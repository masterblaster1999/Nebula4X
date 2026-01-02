#include "nebula4x/core/simulation.h"

#include "simulation_internal.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <limits>
#include <optional>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "nebula4x/core/scenario.h"
#include "nebula4x/core/ai_economy.h"
#include "nebula4x/util/log.h"
#include "nebula4x/util/spatial_index.h"

namespace nebula4x {
namespace {
using sim_internal::kTwoPi;
using sim_internal::ascii_to_lower;
using sim_internal::is_mining_installation;
using sim_internal::mkm_per_day_from_speed;
using sim_internal::push_unique;
using sim_internal::vec_contains;
using sim_internal::sorted_keys;
using sim_internal::faction_has_tech;
using sim_internal::FactionEconomyMultipliers;
using sim_internal::compute_faction_economy_multipliers;
using sim_internal::compute_power_allocation;
} // namespace

void Simulation::tick_ai() {
  // Economic planning for AI factions (research, construction, shipbuilding).
  tick_ai_economy(*this);
  const auto ship_ids = sorted_keys(state_.ships);
  const auto faction_ids = sorted_keys(state_.factions);

  auto orders_empty = [&](Id ship_id) -> bool {
    auto it = state_.ship_orders.find(ship_id);
    if (it == state_.ship_orders.end()) return true;
    const ShipOrders& so = it->second;
    if (!so.queue.empty()) return false;
    // A ship with repeat enabled and remaining refills is not considered idle:
    // its queue will be refilled during tick_ships().
    if (so.repeat && !so.repeat_template.empty() && so.repeat_count_remaining != 0) return false;
    return true;
  };

  auto role_priority = [&](ShipRole r) -> int {
    // Pirates like easy prey first.
    switch (r) {
      case ShipRole::Freighter: return 0;
      case ShipRole::Surveyor: return 1;
      case ShipRole::Combatant: return 2;
      default: return 3;
    }
  };

  auto estimate_eta_days_to_pos = [&](Id start_system_id, Vec2 start_pos_mkm, Id fid, double speed_km_s,
                                     Id goal_system_id, Vec2 goal_pos_mkm) -> double {
    if (speed_km_s <= 0.0) return std::numeric_limits<double>::infinity();

    const auto plan = plan_jump_route_cached(start_system_id, start_pos_mkm, fid, speed_km_s, goal_system_id,
                                            /*restrict_to_discovered=*/true, goal_pos_mkm);
    if (!plan) return std::numeric_limits<double>::infinity();
    return plan->total_eta_days;
  };

  auto issue_auto_refuel = [&](Id ship_id) -> bool {
    Ship* ship = find_ptr(state_.ships, ship_id);
    if (!ship) return false;
    if (!ship->auto_refuel) return false;
    if (!orders_empty(ship_id)) return false;
    if (ship->system_id == kInvalidId) return false;
    if (ship->speed_km_s <= 0.0) return false;

    // Avoid fighting the fleet movement logic. Fleets should be controlled by fleet orders.
    if (fleet_for_ship(ship_id) != kInvalidId) return false;

    const ShipDesign* d = find_design(ship->design_id);
    if (!d) return false;
    const double cap = std::max(0.0, d->fuel_capacity_tons);
    if (cap <= 1e-9) return false;

    if (ship->fuel_tons < 0.0) ship->fuel_tons = cap;
    ship->fuel_tons = std::clamp(ship->fuel_tons, 0.0, cap);

    const double frac = ship->fuel_tons / cap;
    const double threshold = std::clamp(ship->auto_refuel_threshold_fraction, 0.0, 1.0);
    if (frac + 1e-9 >= threshold) return false;

    // If we're already docked at any friendly colony, just wait here: tick_refuel()
    // will top us up when Fuel becomes available.
    const double dock_range = std::max(0.0, cfg_.docking_range_mkm);
    for (Id cid : sorted_keys(state_.colonies)) {
      const Colony* c = find_ptr(state_.colonies, cid);
      if (!c) continue;
      if (c->faction_id != ship->faction_id) continue;
      const Body* b = find_ptr(state_.bodies, c->body_id);
      if (!b) continue;
      if (b->system_id != ship->system_id) continue;
      const double dist = (ship->position_mkm - b->position_mkm).length();
      if (dist <= dock_range + 1e-9) {
        return false;
      }
    }

    Id best_colony_id = kInvalidId;
    double best_eta = std::numeric_limits<double>::infinity();
    bool best_has_fuel = false;

    for (Id cid : sorted_keys(state_.colonies)) {
      const Colony* c = find_ptr(state_.colonies, cid);
      if (!c) continue;
      if (c->faction_id != ship->faction_id) continue;
      const Body* b = find_ptr(state_.bodies, c->body_id);
      if (!b) continue;
      if (b->system_id == kInvalidId) continue;

      const double eta = estimate_eta_days_to_pos(ship->system_id, ship->position_mkm, ship->faction_id,
                                                  ship->speed_km_s, b->system_id, b->position_mkm);
      if (!std::isfinite(eta)) continue;

      const double fuel_avail = [&]() {
        if (auto it = c->minerals.find("Fuel"); it != c->minerals.end()) return std::max(0.0, it->second);
        return 0.0;
      }();
      const bool has_fuel = (fuel_avail > 1e-6);

      if (best_colony_id == kInvalidId) {
        best_colony_id = cid;
        best_eta = eta;
        best_has_fuel = has_fuel;
        continue;
      }

      if (has_fuel != best_has_fuel) {
        if (has_fuel && !best_has_fuel) {
          best_colony_id = cid;
          best_eta = eta;
          best_has_fuel = true;
        }
        continue;
      }

      if (eta + 1e-9 < best_eta) {
        best_colony_id = cid;
        best_eta = eta;
        best_has_fuel = has_fuel;
      }
    }

    if (best_colony_id == kInvalidId) return false;

    const Colony* target_colony = find_ptr(state_.colonies, best_colony_id);
    if (!target_colony) return false;
    const Body* target_body = find_ptr(state_.bodies, target_colony->body_id);
    if (!target_body) return false;
    if (!find_ptr(state_.systems, target_body->system_id)) return false;

    // Multi-system travel if needed.
    if (!issue_travel_to_system(ship_id, target_body->system_id, /*restrict_to_discovered=*/true,
                              target_body->position_mkm)) return false;

    auto& orders = state_.ship_orders[ship_id];
    orders.queue.push_back(MoveToBody{target_body->id});
    return true;
  };


  auto issue_auto_repair = [&](Id ship_id) -> bool {
    Ship* ship = find_ptr(state_.ships, ship_id);
    if (!ship) return false;
    if (!ship->auto_repair) return false;
    if (!orders_empty(ship_id)) return false;
    if (ship->system_id == kInvalidId) return false;
    if (ship->speed_km_s <= 0.0) return false;

    // Avoid fighting the fleet movement logic. Fleets should be controlled by fleet orders.
    if (fleet_for_ship(ship_id) != kInvalidId) return false;

    const ShipDesign* d = find_design(ship->design_id);
    if (!d) return false;
    const double max_hp = std::max(0.0, d->max_hp);
    if (max_hp <= 1e-9) return false;

    ship->hp = std::clamp(ship->hp, 0.0, max_hp);
    const double frac = ship->hp / max_hp;
    const double threshold = std::clamp(ship->auto_repair_threshold_fraction, 0.0, 1.0);
    if (frac + 1e-9 >= threshold) return false;

    // If we're already docked at any friendly shipyard colony, just wait here: tick_repairs()
    // will apply repairs as shipyard capacity becomes available.
    const double dock_range = std::max(0.0, cfg_.docking_range_mkm);
    for (Id cid : sorted_keys(state_.colonies)) {
      const Colony* c = find_ptr(state_.colonies, cid);
      if (!c) continue;
      if (!are_factions_mutual_friendly(ship->faction_id, c->faction_id)) continue;

      const auto it_yard = c->installations.find("shipyard");
      const int yards = (it_yard != c->installations.end()) ? it_yard->second : 0;
      if (yards <= 0) continue;

      const Body* b = find_ptr(state_.bodies, c->body_id);
      if (!b) continue;
      if (b->system_id != ship->system_id) continue;

      const double dist = (ship->position_mkm - b->position_mkm).length();
      if (dist <= dock_range + 1e-9) {
        return false;
      }
    }

    Id best_colony_id = kInvalidId;
    double best_score = std::numeric_limits<double>::infinity();
    int best_yards = 0;

    // Consider any mutual-friendly colony with shipyards.
    for (Id cid : sorted_keys(state_.colonies)) {
      const Colony* c = find_ptr(state_.colonies, cid);
      if (!c) continue;
      if (!are_factions_mutual_friendly(ship->faction_id, c->faction_id)) continue;

      const auto it_yard = c->installations.find("shipyard");
      const int yards = (it_yard != c->installations.end()) ? it_yard->second : 0;
      if (yards <= 0) continue;

      const Body* b = find_ptr(state_.bodies, c->body_id);
      if (!b) continue;
      if (b->system_id == kInvalidId) continue;
      if (!find_ptr(state_.systems, b->system_id)) continue;

      const double eta = estimate_eta_days_to_pos(ship->system_id, ship->position_mkm, ship->faction_id,
                                                  ship->speed_km_s, b->system_id, b->position_mkm);
      if (!std::isfinite(eta)) continue;

      // Roughly estimate total time as travel ETA plus repair time at that colony.
      // Note: repair mineral availability is handled by tick_repairs(); we ignore it here.
      const double per_yard = std::max(0.0, cfg_.repair_hp_per_day_per_shipyard);
      double repair_time = 0.0;
      if (per_yard > 1e-9) {
        repair_time = (max_hp - ship->hp) / (per_yard * static_cast<double>(yards));
      } else {
        repair_time = std::numeric_limits<double>::infinity();
      }

      const double score = eta + repair_time;

      if (best_colony_id == kInvalidId || score + 1e-9 < best_score ||
          (std::abs(score - best_score) <= 1e-9 && yards > best_yards) ||
          (std::abs(score - best_score) <= 1e-9 && yards == best_yards && cid < best_colony_id)) {
        best_colony_id = cid;
        best_score = score;
        best_yards = yards;
      }
    }

    if (best_colony_id == kInvalidId) return false;

    const Colony* target_colony = find_ptr(state_.colonies, best_colony_id);
    if (!target_colony) return false;
    const Body* target_body = find_ptr(state_.bodies, target_colony->body_id);
    if (!target_body) return false;

    // Multi-system travel if needed.
    if (!issue_travel_to_system(ship_id, target_body->system_id, /*restrict_to_discovered=*/true,
                              target_body->position_mkm)) return false;

    auto& orders = state_.ship_orders[ship_id];
    orders.queue.push_back(MoveToBody{target_body->id});
    return true;
  };

  // --- Auto-colonize ---
  //
  // Strategy:
  // - Only consider bodies in systems discovered by the ship's faction.
  // - Avoid bodies that already have a colony.
  // - Avoid assigning multiple colony ships to the same target by tracking
  //   already-targeted bodies from existing ship orders.
  // - Score targets by a blend of habitability, mineral deposits, and ETA.
  std::unordered_set<Id> colonized_bodies;
  colonized_bodies.reserve(state_.colonies.size() * 2 + 8);
  for (const auto& [_, c] : state_.colonies) {
    if (c.body_id != kInvalidId) colonized_bodies.insert(c.body_id);
  }

  std::unordered_map<Id, std::unordered_set<Id>> reserved_colonize_targets;
  reserved_colonize_targets.reserve(faction_ids.size() * 2 + 8);
  for (const auto& [sid, so] : state_.ship_orders) {
    const Ship* sh = find_ptr(state_.ships, sid);
    if (!sh) continue;
    auto& reserved = reserved_colonize_targets[sh->faction_id];
    for (const auto& ord : so.queue) {
      if (const auto* c = std::get_if<ColonizeBody>(&ord)) {
        if (c->body_id != kInvalidId) reserved.insert(c->body_id);
      }
    }
  }

  auto is_body_auto_colonizable = [&](const Body& b) -> bool {
    // Keep the AI from doing obviously nonsensical colonization.
    // Colonies can exist anywhere in the prototype, but auto-colonize should
    // stick to plausible colony targets.
    if (b.type == BodyType::Star) return false;
    if (b.type == BodyType::GasGiant) return false;
    return (b.type == BodyType::Planet || b.type == BodyType::Moon || b.type == BodyType::Asteroid);
  };

  auto total_mineral_deposits = [&](const Body& b) -> double {
    double total = 0.0;
    for (const auto& [_, amt] : b.mineral_deposits) {
      if (amt > 0.0) total += amt;
    }
    return total;
  };

  auto issue_auto_colonize = [&](Id ship_id) -> bool {
    Ship* ship = find_ptr(state_.ships, ship_id);
    if (!ship) return false;
    if (!ship->auto_colonize) return false;
    if (!orders_empty(ship_id)) return false;
    if (ship->system_id == kInvalidId) return false;
    if (ship->speed_km_s <= 0.0) return false;

    // Avoid fighting the fleet movement logic. Fleets should be controlled by fleet orders.
    if (fleet_for_ship(ship_id) != kInvalidId) return false;

    const ShipDesign* d = find_design(ship->design_id);
    if (!d) return false;
    if (d->colony_capacity_millions <= 0.0) return false;

    auto& reserved = reserved_colonize_targets[ship->faction_id];

    Id best_body_id = kInvalidId;
    double best_score = -std::numeric_limits<double>::infinity();

    for (Id bid : sorted_keys(state_.bodies)) {
      const Body* b = find_ptr(state_.bodies, bid);
      if (!b) continue;
      if (b->id == kInvalidId) continue;
      if (b->system_id == kInvalidId) continue;
      if (!find_ptr(state_.systems, b->system_id)) continue;

      if (!is_body_auto_colonizable(*b)) continue;
      if (colonized_bodies.contains(bid)) continue;
      if (reserved.contains(bid)) continue;
      if (!is_system_discovered_by_faction(ship->faction_id, b->system_id)) continue;

      const double hab = std::clamp(body_habitability(bid), 0.0, 1.0);
      const double minerals = std::max(0.0, total_mineral_deposits(*b));
      const double mineral_score = std::log10(minerals + 1.0);

      // Skip targets that are both extremely hostile and resource-poor.
      if (hab < 0.05 && mineral_score < 2.0) continue;  // <~ 100 total deposit

      const double eta = estimate_eta_days_to_pos(ship->system_id, ship->position_mkm, ship->faction_id,
                                                  ship->speed_km_s, b->system_id, b->position_mkm);
      if (!std::isfinite(eta)) continue;

      // Score blend:
      // - Habitability dominates for population-friendly worlds.
      // - Minerals matter via log scale (so huge deposits don't dwarf everything).
      // - ETA discourages sending colony ships on extremely long routes.
      double score = hab * 1000.0 + mineral_score * 100.0 - eta * 5.0;
      if (b->type == BodyType::Planet) score += 20.0;
      if (b->type == BodyType::Moon) score += 10.0;

      if (best_body_id == kInvalidId || score > best_score + 1e-9 ||
          (std::abs(score - best_score) <= 1e-9 && bid < best_body_id)) {
        best_body_id = bid;
        best_score = score;
      }
    }

    if (best_body_id == kInvalidId) return false;

    // Reserve immediately so other colony ships don't pick the same target this tick.
    reserved.insert(best_body_id);

    // Queue the travel + colonize order.
    return issue_colonize_body(ship_id, best_body_id, /*colony_name=*/"", /*restrict_to_discovered=*/true);
  };

  auto issue_auto_explore = [&](Id ship_id) -> bool {
    Ship* ship = find_ptr(state_.ships, ship_id);
    if (!ship) return false;
    if (!orders_empty(ship_id)) return false;
    if (ship->system_id == kInvalidId) return false;
    if (ship->speed_km_s <= 0.0) return false;

    // Avoid fighting the fleet movement logic. Fleets should be controlled by fleet orders.
    if (fleet_for_ship(ship_id) != kInvalidId) return false;

    const Id fid = ship->faction_id;
    const auto* sys = find_ptr(state_.systems, ship->system_id);
    if (!sys) return false;

    std::vector<Id> jps = sys->jump_points;
    std::sort(jps.begin(), jps.end());

    // If we have an undiscovered neighbor, jump now.
    for (Id jp_id : jps) {
      const JumpPoint* jp = find_ptr(state_.jump_points, jp_id);
      if (!jp) continue;
      const JumpPoint* other = find_ptr(state_.jump_points, jp->linked_jump_id);
      if (!other) continue;
      const Id dest_sys = other->system_id;
      if (dest_sys == kInvalidId) continue;
      if (!is_system_discovered_by_faction(fid, dest_sys)) {
        issue_travel_via_jump(ship_id, jp_id);
        return true;
      }
    }

    // Otherwise, route to the nearest *discovered* frontier system (one jump away from an undiscovered neighbor).
    std::unordered_set<Id> visited;
    std::queue<Id> q;
    visited.insert(ship->system_id);
    q.push(ship->system_id);

    Id frontier = kInvalidId;

    while (!q.empty()) {
      const Id cur = q.front();
      q.pop();

      const auto* cs = find_ptr(state_.systems, cur);
      if (!cs) continue;

      std::vector<Id> cs_jps = cs->jump_points;
      std::sort(cs_jps.begin(), cs_jps.end());

      bool is_frontier = false;
      for (Id jp_id : cs_jps) {
        const JumpPoint* jp = find_ptr(state_.jump_points, jp_id);
        if (!jp) continue;
        const JumpPoint* other = find_ptr(state_.jump_points, jp->linked_jump_id);
        if (!other) continue;
        const Id dest_sys = other->system_id;
        if (dest_sys == kInvalidId) continue;
        if (!is_system_discovered_by_faction(fid, dest_sys)) {
          is_frontier = true;
          break;
        }
      }

      if (is_frontier) {
        frontier = cur;
        break;
      }

      for (Id jp_id : cs_jps) {
        const JumpPoint* jp = find_ptr(state_.jump_points, jp_id);
        if (!jp) continue;
        const JumpPoint* other = find_ptr(state_.jump_points, jp->linked_jump_id);
        if (!other) continue;
        const Id dest_sys = other->system_id;
        if (dest_sys == kInvalidId) continue;
        if (!is_system_discovered_by_faction(fid, dest_sys)) continue;
        if (visited.insert(dest_sys).second) q.push(dest_sys);
      }
    }

    if (frontier != kInvalidId && frontier != ship->system_id) {
      return issue_travel_to_system(ship_id, frontier, true);
    }

    return false;
  };

  // --- Ship-level automation: Auto-refuel (fuel safety) ---
  for (Id sid : ship_ids) {
    Ship* sh = find_ptr(state_.ships, sid);
    if (!sh) continue;
    if (!sh->auto_refuel) continue;
    if (!orders_empty(sid)) continue;

    (void)issue_auto_refuel(sid);
  }

  // --- Ship-level automation: Auto-repair (damage safety) ---
  for (Id sid : ship_ids) {
    Ship* sh = find_ptr(state_.ships, sid);
    if (!sh) continue;
    if (!sh->auto_repair) continue;
    if (!orders_empty(sid)) continue;

    (void)issue_auto_repair(sid);
  }

  // --- Ship-level automation: Auto-tanker (fuel logistics) ---
  // Reserve fuel transfer targets that are already being serviced (or en-route) so we don't send
  // multiple automated tankers to the same ship.
  std::unordered_map<Id, std::unordered_set<Id>> reserved_fuel_targets;
  reserved_fuel_targets.reserve(faction_ids.size() * 2 + 4);
  for (const auto& [sid, so] : state_.ship_orders) {
    const Ship* ship = find_ptr(state_.ships, sid);
    if (!ship) continue;
    if (ship->faction_id == kInvalidId) continue;
    for (const auto& ord : so.queue) {
      if (const auto* tf = std::get_if<TransferFuelToShip>(&ord)) {
        if (tf->target_ship_id != kInvalidId) {
          reserved_fuel_targets[ship->faction_id].insert(tf->target_ship_id);
        }
      }
    }
  }

  auto issue_auto_tanker = [&](Id tanker_id) -> bool {
    Ship* tanker = find_ptr(state_.ships, tanker_id);
    if (!tanker) return false;
    if (!tanker->auto_tanker) return false;

    // Auto-tanker is mutually exclusive with mission-style automations.
    if (tanker->auto_explore || tanker->auto_freight || tanker->auto_salvage || tanker->auto_colonize) return false;

    if (!orders_empty(tanker_id)) return false;
    if (tanker->system_id == kInvalidId) return false;
    if (tanker->speed_km_s <= 0.0) return false;

    // Avoid fighting the fleet movement logic. Fleets should be controlled by fleet orders.
    if (fleet_for_ship(tanker_id) != kInvalidId) return false;

    const ShipDesign* td = find_design(tanker->design_id);
    if (!td) return false;
    const double tanker_cap = std::max(0.0, td->fuel_capacity_tons);
    if (tanker_cap <= 1e-9) return false;

    const double reserve_frac = std::clamp(tanker->auto_tanker_reserve_fraction, 0.0, 1.0);
    const double reserve_tons = reserve_frac * tanker_cap;
    const double tanker_fuel = std::clamp((tanker->fuel_tons < 0.0) ? tanker_cap : tanker->fuel_tons, 0.0, tanker_cap);
    const double available = std::max(0.0, tanker_fuel - reserve_tons);

    const double min_transfer = std::max(0.0, cfg_.auto_tanker_min_transfer_tons);
    if (available <= min_transfer + 1e-9) return false;

    const Id fid = tanker->faction_id;
    auto& reserved = reserved_fuel_targets[fid];

    const double req_thresh = std::clamp(cfg_.auto_tanker_request_threshold_fraction, 0.0, 1.0);
    const double fill_target = std::clamp(cfg_.auto_tanker_fill_target_fraction, 0.0, 1.0);

    Id best_target_id = kInvalidId;
    double best_frac = 2.0;
    double best_eta = std::numeric_limits<double>::infinity();

    for (Id tid : ship_ids) {
      if (tid == tanker_id) continue;
      if (reserved.contains(tid)) continue;

      const Ship* target = find_ptr(state_.ships, tid);
      if (!target) continue;
      if (target->faction_id != fid) continue;
      if (target->system_id == kInvalidId) continue;

      // Avoid fighting the fleet movement logic.
      if (fleet_for_ship(tid) != kInvalidId) continue;

      // Only service ships that are idle and not already using colony auto-refuel.
      if (!orders_empty(tid)) continue;
      if (target->auto_refuel) continue;

      const ShipDesign* sd = find_design(target->design_id);
      if (!sd) continue;
      const double cap = std::max(0.0, sd->fuel_capacity_tons);
      if (cap <= 1e-9) continue;

      const double fuel = std::clamp((target->fuel_tons < 0.0) ? cap : target->fuel_tons, 0.0, cap);
      const double frac = (cap > 0.0) ? (fuel / cap) : 1.0;
      if (frac + 1e-9 >= req_thresh) continue;

      // Require the target system to be in our discovered map so route planning is deterministic.
      if (!is_system_discovered_by_faction(fid, target->system_id)) continue;

      const double eta = estimate_eta_days_to_pos(tanker->system_id, tanker->position_mkm, fid, tanker->speed_km_s,
                                                  target->system_id, target->position_mkm);
      if (!std::isfinite(eta)) continue;

      // Primary: lowest fuel fraction.
      // Secondary: shortest ETA.
      // Tertiary: lowest id.
      if (best_target_id == kInvalidId || frac < best_frac - 1e-9 ||
          (std::abs(frac - best_frac) <= 1e-9 && eta < best_eta - 1e-9) ||
          (std::abs(frac - best_frac) <= 1e-9 && std::abs(eta - best_eta) <= 1e-9 && tid < best_target_id)) {
        best_target_id = tid;
        best_frac = frac;
        best_eta = eta;
      }
    }

    if (best_target_id == kInvalidId) return false;

    const Ship* tgt = find_ptr(state_.ships, best_target_id);
    if (!tgt) return false;
    const ShipDesign* td2 = find_design(tgt->design_id);
    if (!td2) return false;
    const double tgt_cap = std::max(0.0, td2->fuel_capacity_tons);
    if (tgt_cap <= 1e-9) return false;

    const double tgt_fuel = std::clamp((tgt->fuel_tons < 0.0) ? tgt_cap : tgt->fuel_tons, 0.0, tgt_cap);
    const double desired = tgt_cap * fill_target;
    const double need = std::max(0.0, desired - tgt_fuel);

    const double give = std::min(available, need);
    if (give <= min_transfer + 1e-9) return false;

    if (!issue_transfer_fuel_to_ship(tanker_id, best_target_id, give, /*restrict_to_discovered=*/true)) return false;

    reserved.insert(best_target_id);
    return true;
  };

  for (Id sid : ship_ids) {
    Ship* sh = find_ptr(state_.ships, sid);
    if (!sh) continue;
    if (!sh->auto_tanker) continue;
    if (!orders_empty(sid)) continue;

    (void)issue_auto_tanker(sid);
  }

  // --- Ship-level automation: Auto-salvage (wreck recovery) ---
  // Reserve wreck targets that are already being salvaged (or en-route) so we don't
  // send multiple automated ships to the same wreck.
  //
  // This mirrors common 4X salvage UX expectations: one ship works a wreck at a time,
  // and additional salvage ships should look for other opportunities.
  std::unordered_map<Id, std::unordered_set<Id>> reserved_wreck_targets;
  reserved_wreck_targets.reserve(faction_ids.size() * 2 + 4);
  for (const auto& [sid, so] : state_.ship_orders) {
    const Ship* ship = find_ptr(state_.ships, sid);
    if (!ship) continue;
    if (ship->faction_id == kInvalidId) continue;
    for (const auto& ord : so.queue) {
      if (const auto* sw = std::get_if<SalvageWreck>(&ord)) {
        if (sw->wreck_id != kInvalidId) reserved_wreck_targets[ship->faction_id].insert(sw->wreck_id);
      }
    }
  }

  const auto wreck_ids = sorted_keys(state_.wrecks);

  auto issue_auto_salvage = [&](Id ship_id) -> bool {
    Ship* ship = find_ptr(state_.ships, ship_id);
    if (!ship) return false;
    if (!ship->auto_salvage) return false;
    if (!orders_empty(ship_id)) return false;
    if (ship->system_id == kInvalidId) return false;
    if (ship->speed_km_s <= 0.0) return false;

    // Avoid fighting the fleet movement logic. Fleets should be controlled by fleet orders.
    if (fleet_for_ship(ship_id) != kInvalidId) return false;

    const ShipDesign* d = find_design(ship->design_id);
    const double cap = d ? std::max(0.0, d->cargo_tons) : 0.0;
    if (cap <= 1e-9) return false;

    auto cargo_used_tons = [&](const Ship& s) {
      double used = 0.0;
      for (const auto& [_, tons] : s.cargo) used += std::max(0.0, tons);
      return used;
    };

    const double used = cargo_used_tons(*ship);

    // 1) If we're carrying anything, deliver it to the nearest friendly colony.
    if (used > 1e-6) {
      Id best_colony_id = kInvalidId;
      double best_eta = std::numeric_limits<double>::infinity();

      for (Id cid : sorted_keys(state_.colonies)) {
        const Colony* c = find_ptr(state_.colonies, cid);
        if (!c) continue;
        if (c->faction_id != ship->faction_id) continue;
        const Body* b = find_ptr(state_.bodies, c->body_id);
        if (!b) continue;
        if (b->system_id == kInvalidId) continue;

        const double eta = estimate_eta_days_to_pos(ship->system_id, ship->position_mkm, ship->faction_id,
                                                  ship->speed_km_s, b->system_id, b->position_mkm);
        if (!std::isfinite(eta)) continue;
        if (eta < best_eta) {
          best_eta = eta;
          best_colony_id = cid;
        }
      }

      if (best_colony_id == kInvalidId) return false;

      // Unload all cargo minerals.
      return issue_unload_mineral(ship_id, best_colony_id, /*mineral=*/"", /*tons=*/0.0,
                                 /*restrict_to_discovered=*/true);
    }

    // 2) Otherwise, find the best available wreck in discovered space.
    const Id fid = ship->faction_id;
    auto& reserved = reserved_wreck_targets[fid];

    Id best_wreck_id = kInvalidId;
    double best_score = -std::numeric_limits<double>::infinity();
    double best_eta = std::numeric_limits<double>::infinity();
    double best_total = 0.0;

    for (Id wid : wreck_ids) {
      const Wreck* w = find_ptr(state_.wrecks, wid);
      if (!w) continue;
      if (w->system_id == kInvalidId) continue;

      // Honor fog-of-war: auto-salvage only operates inside discovered space.
      if (!is_system_discovered_by_faction(fid, w->system_id)) continue;

      if (reserved.find(wid) != reserved.end()) continue;

      double total = 0.0;
      for (const auto& [_, tons] : w->minerals) total += std::max(0.0, tons);
      if (total <= 1e-9) continue;

      const double eta = estimate_eta_days_to_pos(ship->system_id, ship->position_mkm, fid, ship->speed_km_s,
                                                w->system_id, w->position_mkm);
      if (!std::isfinite(eta)) continue;

      // Score: prefer closer wrecks, but strongly bias toward larger returns.
      const double score = std::log10(total + 1.0) * 100.0 - eta;

      if (score > best_score + 1e-9 || (std::abs(score - best_score) <= 1e-9 && (eta < best_eta - 1e-9)) ||
          (std::abs(score - best_score) <= 1e-9 && std::abs(eta - best_eta) <= 1e-9 && total > best_total + 1e-9)) {
        best_score = score;
        best_wreck_id = wid;
        best_eta = eta;
        best_total = total;
      }
    }

    if (best_wreck_id == kInvalidId) return false;

    reserved.insert(best_wreck_id);
    return issue_salvage_wreck(ship_id, best_wreck_id, /*mineral=*/"", /*tons=*/0.0,
                              /*restrict_to_discovered=*/true);
  };

  for (Id sid : ship_ids) {
    Ship* sh = find_ptr(state_.ships, sid);
    if (!sh) continue;
    if (!sh->auto_salvage) continue;
    if (sh->auto_explore) continue;   // mutually exclusive; auto-explore handled below
    if (sh->auto_freight) continue;   // mutually exclusive; auto-freight handled below
    if (sh->auto_colonize) continue;  // mutually exclusive; auto-colonize handled below
    if (sh->auto_tanker) continue;    // mutually exclusive; auto-tanker handled above
    if (!orders_empty(sid)) continue;

    (void)issue_auto_salvage(sid);
  }

  // --- Ship-level automation: Auto-colonize ---
  for (Id sid : ship_ids) {
    Ship* sh = find_ptr(state_.ships, sid);
    if (!sh) continue;
    if (!sh->auto_colonize) continue;
    if (sh->auto_explore) continue;   // mutually exclusive; auto-explore handled below
    if (sh->auto_freight) continue;   // mutually exclusive; auto-freight handled below
    if (sh->auto_salvage) continue;   // mutually exclusive; auto-salvage handled above
    if (sh->auto_tanker) continue;    // mutually exclusive; auto-tanker handled above
    if (!orders_empty(sid)) continue;

    (void)issue_auto_colonize(sid);
  }

  // --- Ship-level automation: Auto-explore ---
  for (Id sid : ship_ids) {
    Ship* sh = find_ptr(state_.ships, sid);
    if (!sh) continue;
    if (!sh->auto_explore) continue;
    if (sh->auto_freight) continue;  // mutually exclusive; auto-freight handled below
    if (sh->auto_colonize) continue; // mutually exclusive; auto-colonize handled above
    if (sh->auto_salvage) continue;  // mutually exclusive; auto-salvage handled above
    if (sh->auto_tanker) continue;   // mutually exclusive; auto-tanker handled above
    if (!orders_empty(sid)) continue;

    (void)issue_auto_explore(sid);
  }


  // --- Ship-level automation: Auto-freight (mineral logistics) ---
  auto cargo_used_tons = [](const Ship& s) {
    double used = 0.0;
    for (const auto& [_, tons] : s.cargo) used += std::max(0.0, tons);
    return used;
  };

  // Group idle auto-freight ships by faction so we can avoid over-assigning the same minerals.
  std::unordered_map<Id, std::vector<Id>> freight_ships_by_faction;
  freight_ships_by_faction.reserve(faction_ids.size() * 2);

  for (Id sid : ship_ids) {
    Ship* sh = find_ptr(state_.ships, sid);
    if (!sh) continue;
    if (!sh->auto_freight) continue;
    if (sh->auto_explore) continue;  // mutually exclusive; auto-explore handled above
    if (sh->auto_colonize) continue; // mutually exclusive; auto-colonize handled above
    if (sh->auto_salvage) continue;  // mutually exclusive; auto-salvage handled above
    if (sh->auto_tanker) continue;   // mutually exclusive; auto-tanker handled above
    if (!orders_empty(sid)) continue;
    if (sh->system_id == kInvalidId) continue;
    if (sh->speed_km_s <= 0.0) continue;

    // Avoid fighting the fleet movement logic. Fleets should be controlled by fleet orders.
    if (fleet_for_ship(sid) != kInvalidId) continue;

    const auto* d = find_design(sh->design_id);
    const double cap = d ? std::max(0.0, d->cargo_tons) : 0.0;
    if (cap <= 1e-9) continue;

    freight_ships_by_faction[sh->faction_id].push_back(sid);
  }

  for (Id fid : faction_ids) {
    auto it_auto = freight_ships_by_faction.find(fid);
    if (it_auto == freight_ships_by_faction.end()) continue;

    // Gather colonies for this faction and their body positions.
    std::vector<Id> colony_ids;
    colony_ids.reserve(state_.colonies.size());
    std::unordered_map<Id, Id> colony_system;
    std::unordered_map<Id, Vec2> colony_pos;
    for (Id cid : sorted_keys(state_.colonies)) {
      const Colony* c = find_ptr(state_.colonies, cid);
      if (!c) continue;
      if (c->faction_id != fid) continue;
      const Body* b = find_ptr(state_.bodies, c->body_id);
      if (!b) continue;
      if (b->system_id == kInvalidId) continue;
      colony_ids.push_back(cid);
      colony_system[cid] = b->system_id;
      colony_pos[cid] = b->position_mkm;
    }

    if (colony_ids.empty()) continue;

    // Compute per-colony mineral reserves (to avoid starving the source colony's own queues),
    // and compute mineral shortfalls that we want to relieve.
    std::unordered_map<Id, std::unordered_map<std::string, double>> reserve_by_colony;
    std::unordered_map<Id, std::unordered_map<std::string, double>> missing_by_colony;
    const auto needs = logistics_needs_for_faction(fid);

    // Seed reserves from user-configured colony reserve settings.
    for (Id cid : colony_ids) {
      const Colony* c = find_ptr(state_.colonies, cid);
      if (!c) continue;
      for (const auto& [mineral, tons_raw] : c->mineral_reserves) {
        const double tons = std::max(0.0, tons_raw);
        if (tons <= 1e-9) continue;
        double& r = reserve_by_colony[cid][mineral];
        r = std::max(r, tons);
      }
    }


    for (const auto& n : needs) {
      // Reserve: keep enough at the colony to satisfy the local target (one day shipyard throughput or one build unit).
      double& r = reserve_by_colony[n.colony_id][n.mineral];
      r = std::max(r, std::max(0.0, n.desired_tons));

      const double missing = std::max(0.0, n.missing_tons);
      if (missing > 1e-9) {
        double& m = missing_by_colony[n.colony_id][n.mineral];
        m = std::max(m, missing);
      }
    }

    // Precompute per-destination mineral priority lists (descending missing tons).
    // This provides deterministic iteration order even though our storage is hash-based.
    std::unordered_map<Id, std::vector<std::string>> need_minerals_by_colony;
    need_minerals_by_colony.reserve(missing_by_colony.size() * 2 + 8);
    for (Id cid : colony_ids) {
      auto it_miss = missing_by_colony.find(cid);
      if (it_miss == missing_by_colony.end()) continue;

      std::vector<std::pair<std::string, double>> pairs;
      pairs.reserve(it_miss->second.size());
      for (const auto& [mineral, miss_raw] : it_miss->second) {
        const double miss = std::max(0.0, miss_raw);
        if (miss <= 1e-9) continue;
        pairs.emplace_back(mineral, miss);
      }
      if (pairs.empty()) continue;

      std::sort(pairs.begin(), pairs.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
      });

      std::vector<std::string> minerals;
      minerals.reserve(pairs.size());
      for (const auto& [m, _] : pairs) minerals.push_back(m);
      need_minerals_by_colony[cid] = std::move(minerals);
    }

    // Stable lists of destinations and sources.
    std::vector<Id> dests_with_needs;
    dests_with_needs.reserve(need_minerals_by_colony.size());
    for (Id cid : colony_ids) {
      if (need_minerals_by_colony.find(cid) != need_minerals_by_colony.end()) dests_with_needs.push_back(cid);
    }

    // Compute exportable minerals for each colony = stockpile - local reserve.
    std::unordered_map<Id, std::unordered_map<std::string, double>> exportable_by_colony;
    exportable_by_colony.reserve(colony_ids.size() * 2);
    for (Id cid : colony_ids) {
      const Colony* c = find_ptr(state_.colonies, cid);
      if (!c) continue;
      for (const auto& [mineral, have_raw] : c->minerals) {
        const double have = std::max(0.0, have_raw);
        double reserve = 0.0;
        if (auto it_r = reserve_by_colony.find(cid); it_r != reserve_by_colony.end()) {
          if (auto it_m = it_r->second.find(mineral); it_m != it_r->second.end()) reserve = std::max(0.0, it_m->second);
        }
        const double surplus = std::max(0.0, have - reserve);
        if (surplus > 1e-9) {
          exportable_by_colony[cid][mineral] = surplus;
        }
      }
    }

    auto auto_ships = it_auto->second;
    std::sort(auto_ships.begin(), auto_ships.end());

    const bool bundle_multi = cfg_.auto_freight_multi_mineral;
    // Avoid degenerate "0 ton" shipments if the config is set to 0.
    const double min_tons = std::max(1e-6, cfg_.auto_freight_min_transfer_tons);
    const double take_frac = std::clamp(cfg_.auto_freight_max_take_fraction_of_surplus, 0.0, 1.0);

    struct FreightItem {
      std::string mineral;
      double tons{0.0};
    };

    auto dec_map_value = [](std::unordered_map<std::string, double>& m, const std::string& key, double amount) {
      if (amount <= 0.0) return;
      auto it = m.find(key);
      if (it == m.end()) return;
      it->second = std::max(0.0, it->second - amount);
      if (it->second <= 1e-9) m.erase(it);
    };

    auto dec_missing = [&](Id cid, const std::string& mineral, double amount) {
      if (amount <= 0.0) return;
      auto itc = missing_by_colony.find(cid);
      if (itc == missing_by_colony.end()) return;
      dec_map_value(itc->second, mineral, amount);
      if (itc->second.empty()) missing_by_colony.erase(itc);
    };

    auto dec_exportable = [&](Id cid, const std::string& mineral, double amount) {
      if (amount <= 0.0) return;
      auto itc = exportable_by_colony.find(cid);
      if (itc == exportable_by_colony.end()) return;
      dec_map_value(itc->second, mineral, amount);
      if (itc->second.empty()) exportable_by_colony.erase(itc);
    };

    for (Id sid : auto_ships) {
      Ship* sh = find_ptr(state_.ships, sid);
      if (!sh) continue;
      if (!orders_empty(sid)) continue;
      if (sh->system_id == kInvalidId) continue;

      const auto* d = find_design(sh->design_id);
      const double cap = d ? std::max(0.0, d->cargo_tons) : 0.0;
      if (cap <= 1e-9) continue;

      const double used = cargo_used_tons(*sh);
      const double free = std::max(0.0, cap - used);

      // 1) If we already have cargo, try to deliver it (optionally bundling multiple minerals)
      //    to a single colony that needs them.
      bool assigned = false;
      if (used > 1e-9 && !dests_with_needs.empty()) {
        std::vector<std::string> cargo_minerals;
        cargo_minerals.reserve(sh->cargo.size());
        for (const auto& [m, tons_raw] : sh->cargo) {
          if (std::max(0.0, tons_raw) > 1e-9) cargo_minerals.push_back(m);
        }
        std::sort(cargo_minerals.begin(), cargo_minerals.end());

        struct UnloadChoice {
          Id dest{kInvalidId};
          double eff{std::numeric_limits<double>::infinity()};
          double eta{std::numeric_limits<double>::infinity()};
          double total{0.0};
          std::vector<FreightItem> items;
        } best;

        for (Id dest_cid : dests_with_needs) {
          if (dest_cid == kInvalidId) continue;
          auto it_sys = colony_system.find(dest_cid);
          auto it_pos = colony_pos.find(dest_cid);
          if (it_sys == colony_system.end() || it_pos == colony_pos.end()) continue;

          std::vector<FreightItem> items;
          items.reserve(bundle_multi ? cargo_minerals.size() : 1);
          double total = 0.0;

          for (const auto& mineral : cargo_minerals) {
            const double have = [&]() {
              auto it = sh->cargo.find(mineral);
              return (it == sh->cargo.end()) ? 0.0 : std::max(0.0, it->second);
            }();
            if (have < min_tons) continue;

            const double miss = [&]() {
              auto itc = missing_by_colony.find(dest_cid);
              if (itc == missing_by_colony.end()) return 0.0;
              auto itm = itc->second.find(mineral);
              if (itm == itc->second.end()) return 0.0;
              return std::max(0.0, itm->second);
            }();
            if (miss < min_tons) continue;

            const double amount = std::min(have, miss);
            if (amount < min_tons) continue;

            items.push_back(FreightItem{mineral, amount});
            total += amount;

            if (!bundle_multi) break;
          }

          if (total < min_tons) continue;
          const double eta = estimate_eta_days_to_pos(sh->system_id, sh->position_mkm, fid, sh->speed_km_s,
                                                     it_sys->second, it_pos->second);
          if (!std::isfinite(eta)) continue;

          const double eff = eta / std::max(1e-9, total);
          if (best.dest == kInvalidId || eff < best.eff - 1e-9 ||
              (std::abs(eff - best.eff) <= 1e-9 && (eta < best.eta - 1e-9 ||
                                                   (std::abs(eta - best.eta) <= 1e-9 &&
                                                    (total > best.total + 1e-9 ||
                                                     (std::abs(total - best.total) <= 1e-9 && dest_cid < best.dest)))))) {
            best.dest = dest_cid;
            best.eff = eff;
            best.eta = eta;
            best.total = total;
            best.items = std::move(items);
          }
        }

        if (best.dest != kInvalidId && !best.items.empty()) {
          bool ok = true;
          for (const auto& it : best.items) {
            ok = ok && issue_unload_mineral(sid, best.dest, it.mineral, it.tons, /*restrict_to_discovered=*/true);
          }
          if (!ok) {
            (void)clear_orders(sid);
          } else {
            for (const auto& it : best.items) {
              dec_missing(best.dest, it.mineral, it.tons);
            }
            assigned = true;
          }
        }
      }

      if (assigned) continue;

      // 2) Otherwise, pick a source colony and destination colony, optionally bundling multiple minerals
      //    that the destination needs in a single trip.
      if (free < min_tons) continue;
      if (dests_with_needs.empty()) continue;
      if (exportable_by_colony.empty()) continue;

      // Candidate source colonies (sorted).
      std::vector<Id> sources;
      sources.reserve(exportable_by_colony.size());
      for (Id cid : colony_ids) {
        if (exportable_by_colony.find(cid) != exportable_by_colony.end()) sources.push_back(cid);
      }

      struct LoadChoice {
        Id source{kInvalidId};
        Id dest{kInvalidId};
        double eff{std::numeric_limits<double>::infinity()};
        double eta_total{std::numeric_limits<double>::infinity()};
        double total{0.0};
        std::vector<FreightItem> items;
      } best;

      for (Id dest_cid : dests_with_needs) {
        auto it_need_list = need_minerals_by_colony.find(dest_cid);
        if (it_need_list == need_minerals_by_colony.end()) continue;
        auto it_dest_sys = colony_system.find(dest_cid);
        auto it_dest_pos = colony_pos.find(dest_cid);
        if (it_dest_sys == colony_system.end() || it_dest_pos == colony_pos.end()) continue;

        for (Id src_cid : sources) {
          if (src_cid == dest_cid) continue;
          auto it_src_sys = colony_system.find(src_cid);
          auto it_src_pos = colony_pos.find(src_cid);
          if (it_src_sys == colony_system.end() || it_src_pos == colony_pos.end()) continue;

          auto it_exp_c = exportable_by_colony.find(src_cid);
          if (it_exp_c == exportable_by_colony.end()) continue;

          std::vector<FreightItem> items;
          items.reserve(bundle_multi ? it_need_list->second.size() : 1);
          double remaining = free;
          double total = 0.0;

          for (const std::string& mineral : it_need_list->second) {
            if (remaining < min_tons) break;

            const double miss = [&]() {
              auto itc = missing_by_colony.find(dest_cid);
              if (itc == missing_by_colony.end()) return 0.0;
              auto itm = itc->second.find(mineral);
              if (itm == itc->second.end()) return 0.0;
              return std::max(0.0, itm->second);
            }();
            if (miss < min_tons) continue;

            auto it_exp = it_exp_c->second.find(mineral);
            if (it_exp == it_exp_c->second.end()) continue;
            const double avail = std::max(0.0, it_exp->second);
            if (avail < min_tons) continue;

            const double take_cap = avail * take_frac;
            const double amount = std::min({remaining, miss, take_cap});
            if (amount < min_tons) continue;

            items.push_back(FreightItem{mineral, amount});
            total += amount;
            remaining -= amount;

            if (!bundle_multi) break;
          }

          if (total < min_tons) continue;

          const double eta1 = estimate_eta_days_to_pos(sh->system_id, sh->position_mkm, fid, sh->speed_km_s,
                                                       it_src_sys->second, it_src_pos->second);
          if (!std::isfinite(eta1)) continue;
          const double eta2 = estimate_eta_days_to_pos(it_src_sys->second, it_src_pos->second, fid, sh->speed_km_s,
                                                       it_dest_sys->second, it_dest_pos->second);
          if (!std::isfinite(eta2)) continue;

          const double eta_total = eta1 + eta2;
          const double eff = eta_total / std::max(1e-9, total);

          if (best.source == kInvalidId || eff < best.eff - 1e-9 ||
              (std::abs(eff - best.eff) <= 1e-9 && (eta_total < best.eta_total - 1e-9 ||
                                                   (std::abs(eta_total - best.eta_total) <= 1e-9 &&
                                                    (total > best.total + 1e-9 ||
                                                     (std::abs(total - best.total) <= 1e-9 &&
                                                      (dest_cid < best.dest ||
                                                       (dest_cid == best.dest && src_cid < best.source)))))))) {
            best.source = src_cid;
            best.dest = dest_cid;
            best.eff = eff;
            best.eta_total = eta_total;
            best.total = total;
            best.items = std::move(items);
          }
        }
      }

      if (best.source != kInvalidId && best.dest != kInvalidId && !best.items.empty()) {
        bool ok = true;
        for (const auto& it : best.items) {
          ok = ok && issue_load_mineral(sid, best.source, it.mineral, it.tons, /*restrict_to_discovered=*/true);
        }
        for (const auto& it : best.items) {
          ok = ok && issue_unload_mineral(sid, best.dest, it.mineral, it.tons, /*restrict_to_discovered=*/true);
        }

        if (!ok) {
          (void)clear_orders(sid);
        } else {
          for (const auto& it : best.items) {
            dec_exportable(best.source, it.mineral, it.tons);
            dec_missing(best.dest, it.mineral, it.tons);
          }
        }
      }
    }
  }

  // --- Faction-level AI profiles ---
  const int now = static_cast<int>(state_.date.days_since_epoch());
  constexpr int kMaxChaseAgeDays = 60;

  for (Id fid : faction_ids) {
    Faction& fac = state_.factions.at(fid);

    if (fac.control == FactionControl::Player) continue;
    if (fac.control == FactionControl::AI_Passive) continue;

    if (fac.control == FactionControl::AI_Explorer) {
      for (Id sid : ship_ids) {
        Ship* sh = find_ptr(state_.ships, sid);
        if (!sh) continue;
        if (sh->faction_id != fid) continue;
        if (!orders_empty(sid)) continue;
        if (sh->auto_explore) continue;  // already handled above
        const auto* d = find_design(sh->design_id);
        if (d && d->role != ShipRole::Surveyor) continue;
        (void)issue_auto_explore(sid);
      }
      continue;
    }

    if (fac.control == FactionControl::AI_Pirate) {
      for (Id sid : ship_ids) {
        Ship* sh = find_ptr(state_.ships, sid);
        if (!sh) continue;
        if (sh->faction_id != fid) continue;
        if (!orders_empty(sid)) continue;
        if (sh->auto_explore) continue;  // allow manual override

        // 1) If hostiles are currently detected in-system, attack the best target.
        const auto hostiles = detected_hostile_ships_in_system(fid, sh->system_id);
        if (!hostiles.empty()) {
          Id best = kInvalidId;
          int best_prio = 999;
          double best_dist = 0.0;

          for (Id tid : hostiles) {
            const Ship* tgt = find_ptr(state_.ships, tid);
            if (!tgt) continue;
            const auto* td = find_design(tgt->design_id);
            const ShipRole tr = td ? td->role : ShipRole::Unknown;
            const int prio = role_priority(tr);
            const double dist = (tgt->position_mkm - sh->position_mkm).length();

            if (best == kInvalidId || prio < best_prio ||
                (prio == best_prio && (dist < best_dist - 1e-9 ||
                                       (std::abs(dist - best_dist) <= 1e-9 && tid < best)))) {
              best = tid;
              best_prio = prio;
              best_dist = dist;
            }
          }

          if (best != kInvalidId) {
            (void)issue_attack_ship(sid, best, true);
            continue;
          }
        }

        // 2) Otherwise, chase a recent hostile contact (last known intel).
        Id contact_target = kInvalidId;
        int best_day = -1;
        int best_prio = 999;

        for (const auto& [_, c] : fac.ship_contacts) {
          if (c.ship_id == kInvalidId) continue;
          if (c.last_seen_faction_id == fid) continue;  // friendly
          if (state_.ships.find(c.ship_id) == state_.ships.end()) continue;
          const int age = now - c.last_seen_day;
          if (age > kMaxChaseAgeDays) continue;
          if (!is_system_discovered_by_faction(fid, c.system_id)) continue;

          const auto* td = find_design(c.last_seen_design_id);
          const ShipRole tr = td ? td->role : ShipRole::Unknown;
          const int prio = role_priority(tr);

          if (c.last_seen_day > best_day || (c.last_seen_day == best_day && prio < best_prio) ||
              (c.last_seen_day == best_day && prio == best_prio && c.ship_id < contact_target)) {
            contact_target = c.ship_id;
            best_day = c.last_seen_day;
            best_prio = prio;
          }
        }

        if (contact_target != kInvalidId) {
          (void)issue_attack_ship(sid, contact_target, true);
          continue;
        }

        // 3) Roam: pick a jump point (prefer exploring undiscovered neighbors).
        const auto* sys = find_ptr(state_.systems, sh->system_id);
        if (!sys) continue;

        std::vector<Id> jps = sys->jump_points;
        std::sort(jps.begin(), jps.end());

        Id chosen = kInvalidId;
        Id fallback = kInvalidId;
        for (Id jp_id : jps) {
          const JumpPoint* jp = find_ptr(state_.jump_points, jp_id);
          if (!jp) continue;
          const JumpPoint* other = find_ptr(state_.jump_points, jp->linked_jump_id);
          if (!other) continue;
          const Id dest_sys = other->system_id;
          if (dest_sys == kInvalidId) continue;

          if (fallback == kInvalidId) fallback = jp_id;
          if (!is_system_discovered_by_faction(fid, dest_sys)) {
            chosen = jp_id;
            break;
          }
        }
        if (chosen == kInvalidId) chosen = fallback;

        if (chosen != kInvalidId) {
          (void)issue_travel_via_jump(sid, chosen);
        }
      }
      continue;
    }
  }
}

void Simulation::tick_refuel() {
  constexpr const char* kFuelKey = "Fuel";

  // Fast(ish) lookup: system -> colony ids.
  std::unordered_map<Id, std::vector<Id>> colonies_in_system;
  colonies_in_system.reserve(state_.systems.size() * 2 + 8);

  for (const auto& [cid, col] : state_.colonies) {
    const auto* body = find_ptr(state_.bodies, col.body_id);
    if (!body) continue;
    colonies_in_system[body->system_id].push_back(cid);
  }

  const double arrive_eps = std::max(0.0, cfg_.arrival_epsilon_mkm);
  const double dock_range = std::max(arrive_eps, cfg_.docking_range_mkm);

  for (auto& [sid, ship] : state_.ships) {
    const ShipDesign* d = find_design(ship.design_id);
    if (!d) continue;

    const double cap = std::max(0.0, d->fuel_capacity_tons);
    if (cap <= 1e-9) continue;

    // Clamp away any weird negative sentinel states before using.
    ship.fuel_tons = std::clamp(ship.fuel_tons, 0.0, cap);

    const double need = cap - ship.fuel_tons;
    if (need <= 1e-9) continue;

    auto it = colonies_in_system.find(ship.system_id);
    if (it == colonies_in_system.end()) continue;

    Id best_cid = kInvalidId;
    double best_dist = 1e100;

    for (Id cid : it->second) {
      const Colony* col = find_ptr(state_.colonies, cid);
      if (!col) continue;
      if (!are_factions_mutual_friendly(ship.faction_id, col->faction_id)) continue;

      const Body* body = find_ptr(state_.bodies, col->body_id);
      if (!body) continue;
      const double dist = (body->position_mkm - ship.position_mkm).length();
      if (dist > dock_range + 1e-9) continue;

      if (dist < best_dist) {
        best_dist = dist;
        best_cid = cid;
      }
    }

    if (best_cid == kInvalidId) continue;

    Colony& col = state_.colonies.at(best_cid);
    const double avail = col.minerals[kFuelKey];
    if (avail <= 1e-9) continue;

    const double take = std::min(need, avail);
    ship.fuel_tons += take;
    col.minerals[kFuelKey] = avail - take;
    if (col.minerals[kFuelKey] <= 1e-9) col.minerals[kFuelKey] = 0.0;
  }
}


void Simulation::tick_repairs(double dt_days) {
  if (dt_days <= 0.0) return;
  const double per_yard = std::max(0.0, cfg_.repair_hp_per_day_per_shipyard);
  if (per_yard <= 0.0) return;

  const double dock_range = std::max(0.0, cfg_.docking_range_mkm);

  const double cost_dur = std::max(0.0, cfg_.repair_duranium_per_hp);
  const double cost_neu = std::max(0.0, cfg_.repair_neutronium_per_hp);

  // Assign each damaged ship to the *single* best docked shipyard colony (most yards, then closest).
  // This avoids a ship being repaired multiple times in one tick when multiple colonies are within docking range.
  std::unordered_map<Id, std::vector<Id>> ships_by_colony;
  ships_by_colony.reserve(state_.colonies.size() * 2);

  const auto ship_ids = sorted_keys(state_.ships);
  const auto colony_ids = sorted_keys(state_.colonies);

  for (Id sid : ship_ids) {
    auto* ship = find_ptr(state_.ships, sid);
    if (!ship) continue;

    const auto* d = find_design(ship->design_id);
    const double max_hp = d ? d->max_hp : ship->hp;
    if (max_hp <= 0.0) continue;

    // Clamp just in case something drifted out of bounds (custom content, etc.).
    ship->hp = std::clamp(ship->hp, 0.0, max_hp);
    if (ship->hp >= max_hp - 1e-9) continue;

    Id best_colony = kInvalidId;
    int best_shipyards = 0;
    double best_dist = 0.0;

    for (Id cid : colony_ids) {
      const auto* colony = find_ptr(state_.colonies, cid);
      if (!colony) continue;
      if (!are_factions_mutual_friendly(ship->faction_id, colony->faction_id)) continue;

      const auto it_yard = colony->installations.find("shipyard");
      const int yards = (it_yard != colony->installations.end()) ? it_yard->second : 0;
      if (yards <= 0) continue;

      const auto* body = find_ptr(state_.bodies, colony->body_id);
      if (!body) continue;
      if (body->system_id != ship->system_id) continue;

      const double dist = (ship->position_mkm - body->position_mkm).length();
      if (dist > dock_range + 1e-9) continue;

      // Prefer the colony with the most shipyards, then the closest distance, then lowest id.
      bool better = false;
      if (yards > best_shipyards) {
        better = true;
      } else if (yards == best_shipyards) {
        if (best_colony == kInvalidId || dist < best_dist - 1e-9) {
          better = true;
        } else if (std::abs(dist - best_dist) <= 1e-9 && cid < best_colony) {
          better = true;
        }
      }

      if (better) {
        best_colony = cid;
        best_shipyards = yards;
        best_dist = dist;
      }
    }

    if (best_colony == kInvalidId || best_shipyards <= 0) continue;
    ships_by_colony[best_colony].push_back(sid);
  }

  if (ships_by_colony.empty()) return;

  auto prio_rank = [](RepairPriority p) -> int {
    switch (p) {
      case RepairPriority::High: return 0;
      case RepairPriority::Normal: return 1;
      case RepairPriority::Low: return 2;
    }
    return 1;
  };

  // Process colonies in deterministic order.
  for (Id cid : colony_ids) {
    auto it_list = ships_by_colony.find(cid);
    if (it_list == ships_by_colony.end()) continue;

    auto* colony = find_ptr(state_.colonies, cid);
    if (!colony) continue;

    const auto it_yard = colony->installations.find("shipyard");
    const int yards = (it_yard != colony->installations.end()) ? it_yard->second : 0;
    if (yards <= 0) continue;

    double capacity = per_yard * static_cast<double>(yards) * dt_days;
    if (capacity <= 1e-9) continue;

    // Apply mineral limits (if configured).
    auto mineral_avail = [&](const std::string& k) {
      auto it = colony->minerals.find(k);
      if (it == colony->minerals.end()) return 0.0;
      return std::max(0.0, it->second);
    };

    if (cost_dur > 1e-12) {
      const double avail = mineral_avail("Duranium");
      capacity = std::min(capacity, avail / cost_dur);
    }
    if (cost_neu > 1e-12) {
      const double avail = mineral_avail("Neutronium");
      capacity = std::min(capacity, avail / cost_neu);
    }

    if (capacity <= 1e-9) continue;

    auto& list = it_list->second;
    std::sort(list.begin(), list.end(), [&](Id a, Id b) {
      const Ship* sa = find_ptr(state_.ships, a);
      const Ship* sb = find_ptr(state_.ships, b);
      const int pa = sa ? prio_rank(sa->repair_priority) : 1;
      const int pb = sb ? prio_rank(sb->repair_priority) : 1;
      if (pa != pb) return pa < pb;
      return a < b;
    });

    double remaining = capacity;
    double applied_total = 0.0;

    for (Id sid : list) {
      if (remaining <= 1e-9) break;

      auto* ship = find_ptr(state_.ships, sid);
      if (!ship) continue;

      const auto* d = find_design(ship->design_id);
      const double max_hp = d ? d->max_hp : ship->hp;
      if (max_hp <= 0.0) continue;

      ship->hp = std::clamp(ship->hp, 0.0, max_hp);
      if (ship->hp >= max_hp - 1e-9) continue;

      const double before = ship->hp;
      const double missing = max_hp - ship->hp;
      const double apply = std::min(remaining, missing);
      ship->hp = std::min(max_hp, ship->hp + apply);

      const double applied = ship->hp - before;
      if (applied <= 0.0) continue;

      remaining -= applied;
      applied_total += applied;

      if (before < max_hp - 1e-9 && ship->hp >= max_hp - 1e-9) {
        // Log only when the ship is fully repaired to avoid event spam.
        const auto* sys = find_ptr(state_.systems, ship->system_id);

        EventContext ctx;
        ctx.faction_id = ship->faction_id;
        ctx.system_id = ship->system_id;
        ctx.ship_id = ship->id;
        ctx.colony_id = cid;

        std::string msg = "Ship repaired: " + ship->name;
        msg += " at " + colony->name;
        if (sys) msg += " in " + sys->name;
        push_event(EventLevel::Info, EventCategory::Shipyard, std::move(msg), ctx);
      }
    }

    if (applied_total <= 1e-9) continue;

    // Consume repair minerals.
    if (cost_dur > 1e-12) {
      double& dur = colony->minerals["Duranium"];
      dur = std::max(0.0, dur - applied_total * cost_dur);
    }
    if (cost_neu > 1e-12) {
      double& neu = colony->minerals["Neutronium"];
      neu = std::max(0.0, neu - applied_total * cost_neu);
    }
  }
}

} // namespace nebula4x
