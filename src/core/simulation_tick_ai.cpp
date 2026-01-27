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
#include "nebula4x/core/fuel_planner.h"
#include "nebula4x/core/freight_planner.h"
#include "nebula4x/core/ground_battle_forecast.h"
#include "nebula4x/core/trade_network.h"
#include "nebula4x/core/troop_planner.h"
#include "nebula4x/core/colonist_planner.h"
#include "nebula4x/util/log.h"
#include "nebula4x/util/trace_events.h"
#include "nebula4x/util/spatial_index.h"
#include "nebula4x/util/hash_rng.h"

namespace nebula4x {
namespace {
using sim_internal::kTwoPi;
using sim_internal::ascii_to_lower;
using sim_internal::is_mining_installation;
using sim_internal::mkm_per_day_from_speed;
using sim_internal::push_unique;
using sim_internal::vec_contains;
using sim_internal::sorted_keys;
using sim_internal::stable_sum_nonneg_sorted_ld;
using sim_internal::faction_has_tech;
using sim_internal::FactionEconomyMultipliers;
using sim_internal::compute_faction_economy_multipliers;
using sim_internal::compute_power_allocation;

// Small, deterministic RNG helpers (platform-stable) for simulation-side
// procedural events.
static double u01(std::uint64_t& s) {
  const std::uint64_t v = ::nebula4x::util::next_splitmix64(s);
  return ::nebula4x::util::u01_from_u64(v);
}

static std::size_t rand_index(std::uint64_t& s, std::size_t n) {
  if (n <= 1) return 0;
  return static_cast<std::size_t>(::nebula4x::util::bounded_u64(s, static_cast<std::uint64_t>(n)));
}


static bool is_player_faction(const GameState& s, Id faction_id) {
  const auto* fac = find_ptr(s.factions, faction_id);
  return fac && fac->control == FactionControl::Player;
}


static double cargo_used_tons(const Ship& s) {
  // Deterministic sum: cargo is an unordered_map and floating-point accumulation
  // order can affect AI decisions near thresholds.
  return static_cast<double>(stable_sum_nonneg_sorted_ld(s.cargo));
}

} // namespace

void Simulation::run_ai_planning() { tick_ai(); }

void Simulation::tick_ai() {
  NEBULA4X_TRACE_SCOPE("tick_ai", "sim.ai");
  // Economic planning for AI factions (research, construction, shipbuilding).
  tick_ai_economy(*this);
  const auto ship_ids = sorted_keys(state_.ships);
  const auto faction_ids = sorted_keys(state_.factions);

  auto orders_empty = [&](Id ship_id) -> bool {
    auto it = state_.ship_orders.find(ship_id);
    if (it == state_.ship_orders.end()) return true;
    const ShipOrders& so = it->second;
    if (so.suspended) return false;
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
    const double burn = std::max(0.0, d->fuel_use_per_mkm);
    const double cap = std::max(0.0, d->fuel_capacity_tons);
    if (cap <= 1e-9) return false;

    if (ship->fuel_tons < 0.0) ship->fuel_tons = cap;
    ship->fuel_tons = std::clamp(ship->fuel_tons, 0.0, cap);

    const double frac = ship->fuel_tons / cap;
    const double threshold = std::clamp(ship->auto_refuel_threshold_fraction, 0.0, 1.0);
    if (frac + 1e-9 >= threshold) return false;

    // If we're already docked at any trade-partner colony, just wait here: tick_refuel()
    // will top us up when Fuel becomes available.
    const double dock_range = std::max(0.0, cfg_.docking_range_mkm);
    for (Id cid : sorted_keys(state_.colonies)) {
      const Colony* c = find_ptr(state_.colonies, cid);
      if (!c) continue;
      if (!are_factions_trade_partners(ship->faction_id, c->faction_id)) continue;
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
      if (!are_factions_trade_partners(ship->faction_id, c->faction_id)) continue;
      const Body* b = find_ptr(state_.bodies, c->body_id);
      if (!b) continue;
      if (b->system_id == kInvalidId) continue;


      auto plan_opt = plan_jump_route_cached(ship->system_id, ship->position_mkm, ship->faction_id,
                                            ship->speed_km_s, b->system_id, true, b->position_mkm);
      if (!plan_opt) continue;
      const double eta = plan_opt->total_eta_days;
      if (!std::isfinite(eta)) continue;

      // Fuel reachability check: avoid routing to a refuel colony we cannot reach.
      if (burn > 0.0) {
        const double fuel_needed = plan_opt->total_distance_mkm * burn;
        if (ship->fuel_tons + 1e-6 < fuel_needed) {
          continue;
        }
      }

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


  auto issue_auto_rearm = [&](Id ship_id) -> bool {
    Ship* ship = find_ptr(state_.ships, ship_id);
    if (!ship) return false;
    if (!ship->auto_rearm) return false;
    if (!orders_empty(ship_id)) return false;
    if (ship->system_id == kInvalidId) return false;
    if (ship->speed_km_s <= 0.0) return false;

    // Avoid fighting the fleet movement logic. Fleets should be controlled by fleet orders.
    if (fleet_for_ship(ship_id) != kInvalidId) return false;

    const ShipDesign* d = find_design(ship->design_id);
    if (!d) return false;
    const double burn = std::max(0.0, d->fuel_use_per_mkm);
    const int cap = std::max(0, d->missile_ammo_capacity);
    if (cap <= 0) return false;

    int ammo = ship->missile_ammo;
    if (ammo < 0) ammo = cap;
    ammo = std::clamp(ammo, 0, cap);

    const double threshold = std::clamp(ship->auto_rearm_threshold_fraction, 0.0, 1.0);

    // If we're not actually low (or have no need), do nothing.
    const int need = cap - ammo;
    if (need <= 0) return false;

    // Account for immediate reload from carried munitions (ammo tenders / cargo holds).
    constexpr const char* kMunitionsKey = "Munitions";
    int ammo_after = ammo;
    if (auto itc = ship->cargo.find(kMunitionsKey); itc != ship->cargo.end()) {
      const int avail = static_cast<int>(std::floor(std::max(0.0, itc->second) + 1e-9));
      ammo_after = std::min(cap, ammo_after + std::min(need, avail));
    }

    const double frac_after = static_cast<double>(ammo_after) / static_cast<double>(cap);
    if (frac_after + 1e-9 >= threshold) return false;

    // If we're already docked at any trade-partner colony, just wait: tick_rearm() will top us up
    // when Munitions are available (possibly via auto-freight).
    const double dock_range = std::max(0.0, cfg_.docking_range_mkm);
    for (Id cid : sorted_keys(state_.colonies)) {
      const Colony* c = find_ptr(state_.colonies, cid);
      if (!c) continue;
      if (!are_factions_trade_partners(ship->faction_id, c->faction_id)) continue;
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
    bool best_has_mun = false;

    for (Id cid : sorted_keys(state_.colonies)) {
      const Colony* c = find_ptr(state_.colonies, cid);
      if (!c) continue;
      if (!are_factions_trade_partners(ship->faction_id, c->faction_id)) continue;
      const Body* b = find_ptr(state_.bodies, c->body_id);
      if (!b) continue;
      if (b->system_id == kInvalidId) continue;


      auto plan_opt = plan_jump_route_cached(ship->system_id, ship->position_mkm, ship->faction_id,
                                            ship->speed_km_s, b->system_id, true, b->position_mkm);
      if (!plan_opt) continue;
      const double eta = plan_opt->total_eta_days;
      if (!std::isfinite(eta)) continue;

      // Fuel reachability check: avoid routing to a refuel colony we cannot reach.
      if (burn > 0.0) {
        const double fuel_needed = plan_opt->total_distance_mkm * burn;
        if (ship->fuel_tons + 1e-6 < fuel_needed) {
          continue;
        }
      }

      const double mun_avail = [&]() {
        if (auto it = c->minerals.find(kMunitionsKey); it != c->minerals.end()) return std::max(0.0, it->second);
        return 0.0;
      }();
      const bool has_mun = (mun_avail >= 1.0 - 1e-9);

      if (best_colony_id == kInvalidId) {
        best_colony_id = cid;
        best_eta = eta;
        best_has_mun = has_mun;
        continue;
      }

      if (has_mun != best_has_mun) {
        if (has_mun && !best_has_mun) {
          best_colony_id = cid;
          best_eta = eta;
          best_has_mun = true;
        }
        continue;
      }

      if (eta + 1e-9 < best_eta) {
        best_colony_id = cid;
        best_eta = eta;
        best_has_mun = has_mun;
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
    const double burn = std::max(0.0, d->fuel_use_per_mkm);
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
      if (!are_factions_trade_partners(ship->faction_id, c->faction_id)) continue;

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

    // Consider any trade-partner colony with shipyards.
    for (Id cid : sorted_keys(state_.colonies)) {
      const Colony* c = find_ptr(state_.colonies, cid);
      if (!c) continue;
      if (!are_factions_trade_partners(ship->faction_id, c->faction_id)) continue;

      const auto it_yard = c->installations.find("shipyard");
      const int yards = (it_yard != c->installations.end()) ? it_yard->second : 0;
      if (yards <= 0) continue;

      const Body* b = find_ptr(state_.bodies, c->body_id);
      if (!b) continue;
      if (b->system_id == kInvalidId) continue;
      if (!find_ptr(state_.systems, b->system_id)) continue;


      auto plan_opt = plan_jump_route_cached(ship->system_id, ship->position_mkm, ship->faction_id,
                                            ship->speed_km_s, b->system_id, true, b->position_mkm);
      if (!plan_opt) continue;
      const double eta = plan_opt->total_eta_days;
      if (!std::isfinite(eta)) continue;

      // Fuel reachability check: avoid routing to a refuel colony we cannot reach.
      if (burn > 0.0) {
        const double fuel_needed = plan_opt->total_distance_mkm * burn;
        if (ship->fuel_tons + 1e-6 < fuel_needed) {
          continue;
        }
      }

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
    return static_cast<double>(stable_sum_nonneg_sorted_ld(b.mineral_deposits));
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
    const double burn = std::max(0.0, d->fuel_use_per_mkm);
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


      auto plan_opt = plan_jump_route_cached(ship->system_id, ship->position_mkm, ship->faction_id,
                                            ship->speed_km_s, b->system_id, true, b->position_mkm);
      if (!plan_opt) continue;
      const double eta = plan_opt->total_eta_days;
      if (!std::isfinite(eta)) continue;

      // Fuel reachability check: avoid routing to a refuel colony we cannot reach.
      if (burn > 0.0) {
        const double fuel_needed = plan_opt->total_distance_mkm * burn;
        if (ship->fuel_tons + 1e-6 < fuel_needed) {
          continue;
        }
      }

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

  // --- Auto-explore ---
  //
  // Strategy:
  // - Never "peek" through unsurveyed jump points. Treat them as unknown exits and
  //   move to them first to survey (fog-of-war friendly).
  // - Prefer transiting through *surveyed* jump points that lead to undiscovered systems.
  // - If the current system has no exploration work, route to a frontier system:
  //   a discovered system that still has unknown exits or known exits to undiscovered systems.
  //
  // Coordination:
  // - Maintain per-faction reservations so multiple idle auto-explore ships will
  //   spread across different exits/frontiers in the same AI tick.
  struct ExploreFrontierInfo {
    Id system_id = kInvalidId;
    int unknown_exits = 0;
    int known_exits_to_undiscovered = 0;

    int weight() const { return unknown_exits + known_exits_to_undiscovered * 2; }
    bool is_frontier() const { return (unknown_exits + known_exits_to_undiscovered) > 0; }
  };

  struct ExploreFactionCache {
    std::unordered_set<Id> discovered;
    std::unordered_set<Id> surveyed;
    std::vector<ExploreFrontierInfo> frontiers;  // deterministic order (system_id ascending)
  };

  std::unordered_map<Id, ExploreFactionCache> explore_cache;
  explore_cache.reserve(faction_ids.size() * 2 + 8);

  for (Id fid : faction_ids) {
    const auto* fac = find_ptr(state_.factions, fid);
    if (!fac) continue;

    ExploreFactionCache c;
    c.discovered.reserve(fac->discovered_systems.size() * 2 + 8);
    for (Id sid : fac->discovered_systems) {
      if (sid != kInvalidId) c.discovered.insert(sid);
    }

    c.surveyed.reserve(fac->surveyed_jump_points.size() * 2 + 8);
    for (Id jid : fac->surveyed_jump_points) {
      if (jid != kInvalidId) c.surveyed.insert(jid);
    }

    // Build deterministic frontier list.
    std::vector<Id> sys_ids;
    sys_ids.reserve(c.discovered.size());
    for (Id sid : c.discovered) sys_ids.push_back(sid);
    std::sort(sys_ids.begin(), sys_ids.end());

    for (Id sys_id : sys_ids) {
      const auto* sys = find_ptr(state_.systems, sys_id);
      if (!sys) continue;

      ExploreFrontierInfo info;
      info.system_id = sys_id;

      // Deterministic scan (stable even if sys->jump_points is unsorted).
      std::vector<Id> jps = sys->jump_points;
      std::sort(jps.begin(), jps.end());

      for (Id jp_id : jps) {
        if (jp_id == kInvalidId) continue;
        const auto* jp = find_ptr(state_.jump_points, jp_id);
        if (!jp || jp->linked_jump_id == kInvalidId) continue;

        if (!c.surveyed.contains(jp_id)) {
          info.unknown_exits += 1;
          continue;
        }

        const auto* other = find_ptr(state_.jump_points, jp->linked_jump_id);
        if (!other) continue;
        const Id dest_sys = other->system_id;
        if (dest_sys == kInvalidId) continue;

        if (!c.discovered.contains(dest_sys)) info.known_exits_to_undiscovered += 1;
      }

      if (info.is_frontier()) c.frontiers.push_back(info);
    }

    explore_cache.emplace(fid, std::move(c));
  }

  std::unordered_map<Id, std::unordered_set<Id>> reserved_explore_jump_targets;
  reserved_explore_jump_targets.reserve(faction_ids.size() * 2 + 8);

  std::unordered_map<Id, std::unordered_set<Id>> reserved_explore_frontier_targets;
  reserved_explore_frontier_targets.reserve(faction_ids.size() * 2 + 8);
  std::unordered_map<Id, std::unordered_set<Id>> reserved_explore_anomaly_targets;
  reserved_explore_anomaly_targets.reserve(faction_ids.size() * 2 + 8);

  std::unordered_map<Id, std::unordered_set<Id>> reserved_explore_wreck_targets;
  reserved_explore_wreck_targets.reserve(faction_ids.size() * 2 + 8);

  std::unordered_map<Id, std::unordered_set<Id>> reserved_explore_bounty_targets;
  reserved_explore_bounty_targets.reserve(faction_ids.size() * 2 + 8);

  // Reserve targets that are already being handled by an active contract assignment.
  // This prevents multiple auto-explore ships from duplicating the same work.
  if (cfg_.enable_contracts && !state_.contracts.empty()) {
    for (const auto& [_, c] : state_.contracts) {
      if (c.assignee_faction_id == kInvalidId || c.target_id == kInvalidId) continue;
      if (c.status != ContractStatus::Accepted && c.status != ContractStatus::Offered) continue;
      if (c.assigned_ship_id == kInvalidId && c.assigned_fleet_id == kInvalidId) continue;

      switch (c.kind) {
        case ContractKind::InvestigateAnomaly:
          reserved_explore_anomaly_targets[c.assignee_faction_id].insert(c.target_id);
          break;
        case ContractKind::SalvageWreck:
          reserved_explore_wreck_targets[c.assignee_faction_id].insert(c.target_id);
          break;
        case ContractKind::SurveyJumpPoint:
          reserved_explore_jump_targets[c.assignee_faction_id].insert(c.target_id);
          break;
        case ContractKind::BountyPirate:
          reserved_explore_bounty_targets[c.assignee_faction_id].insert(c.target_id);
          break;
        case ContractKind::EscortConvoy:
          // Escort contracts are handled by combat/civilian escort logic, not auto-explore.
          break;
      }
    }
  }

  // A system-level ETA helper (no specific goal position; just "get into the system").
  auto estimate_eta_days_to_system = [&](Id start_system_id, Vec2 start_pos_mkm, Id fid, double speed_km_s,
                                        Id goal_system_id) -> double {
    if (speed_km_s <= 0.0) return std::numeric_limits<double>::infinity();
    const auto plan = plan_jump_route_cached(start_system_id, start_pos_mkm, fid, speed_km_s, goal_system_id,
                                            /*restrict_to_discovered=*/true, std::nullopt);
    if (!plan) return std::numeric_limits<double>::infinity();
    return plan->total_eta_days;
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
    const auto* ship_design = find_design(ship->design_id);
    const double ship_weapons = ship_design ? (std::max(0.0, ship_design->weapon_damage) +
                                               std::max(0.0, ship_design->missile_damage))
                                            : 0.0;
    const auto* sys = find_ptr(state_.systems, ship->system_id);
    if (!sys) return false;

    const auto it_cache = explore_cache.find(fid);
    const ExploreFactionCache* cache = (it_cache != explore_cache.end()) ? &it_cache->second : nullptr;

    auto& reserved_jumps = reserved_explore_jump_targets[fid];
    auto& reserved_frontiers = reserved_explore_frontier_targets[fid];
    auto& reserved_anoms = reserved_explore_anomaly_targets[fid];
    auto& reserved_wrecks = reserved_explore_wreck_targets[fid];
    auto& reserved_bounties = reserved_explore_bounty_targets[fid];

    // Contracts: if a ship is idle and has a mission-board assignment (or there is
    // an available unassigned contract for its faction), prefer fulfilling that
    // before generic exploration behavior.
    if (cfg_.enable_contracts && !state_.contracts.empty()) {
      const bool allow_auto_accept = !is_player_faction(state_, fid);

      auto reserve_contract_target = [&](const Contract& c) {
        if (c.target_id == kInvalidId) return;
        switch (c.kind) {
          case ContractKind::InvestigateAnomaly: reserved_anoms.insert(c.target_id); break;
          case ContractKind::SalvageWreck: reserved_wrecks.insert(c.target_id); break;
          case ContractKind::SurveyJumpPoint: reserved_jumps.insert(c.target_id); break;
          case ContractKind::BountyPirate: reserved_bounties.insert(c.target_id); break;
          case ContractKind::EscortConvoy: break;
        }
      };

      auto contract_goal = [&](const Contract& c, Id* out_sys, Vec2* out_pos) -> bool {
        if (out_sys) *out_sys = kInvalidId;
        if (out_pos) *out_pos = Vec2{0.0, 0.0};
        if (c.system_id == kInvalidId || c.target_id == kInvalidId) return false;

        switch (c.kind) {
          case ContractKind::InvestigateAnomaly: {
            const auto* a = find_ptr(state_.anomalies, c.target_id);
            if (!a || a->resolved) return false;
            if (out_sys) *out_sys = a->system_id;
            if (out_pos) *out_pos = a->position_mkm;
            return a->system_id != kInvalidId;
          }
          case ContractKind::SalvageWreck: {
            const auto* w = find_ptr(state_.wrecks, c.target_id);
            if (!w) return false;
            if (out_sys) *out_sys = w->system_id;
            if (out_pos) *out_pos = w->position_mkm;
            return w->system_id != kInvalidId;
          }
          case ContractKind::SurveyJumpPoint: {
            if (is_jump_point_surveyed_by_faction(fid, c.target_id)) return false;
            const auto* jp = find_ptr(state_.jump_points, c.target_id);
            if (!jp) return false;
            if (out_sys) *out_sys = jp->system_id;
            if (out_pos) *out_pos = jp->position_mkm;
            return jp->system_id != kInvalidId;
          }
          case ContractKind::BountyPirate: {
            if (c.target_destroyed_day != 0) return false;

            // Prefer live detections for pursuit. Otherwise use the last seen
            // contact location or the contract's stored system.
            if (is_ship_detected_by_faction(fid, c.target_id)) {
              const auto* sh = find_ptr(state_.ships, c.target_id);
              if (!sh || sh->hp <= 0.0) return false;
              if (out_sys) *out_sys = sh->system_id;
              if (out_pos) *out_pos = sh->position_mkm;
              return sh->system_id != kInvalidId;
            }

            if (const auto* fac = find_ptr(state_.factions, fid)) {
              if (auto it = fac->ship_contacts.find(c.target_id); it != fac->ship_contacts.end()) {
                const Contact& ct = it->second;
                if (out_sys) *out_sys = ct.system_id;
                if (out_pos) *out_pos = ct.last_seen_position_mkm;
                return ct.system_id != kInvalidId;
              }
            }

            if (out_sys) *out_sys = c.system_id;
            if (out_pos) *out_pos = Vec2{0.0, 0.0};
            return c.system_id != kInvalidId;
          }
          case ContractKind::EscortConvoy: {
            // Auto-explore ships should not attempt to fulfill escort contracts.
            return false;
          }
        }
        return false;
      };

      // (0) If this ship is already assigned to a contract, ensure its orders exist.
      for (auto& [cid, c] : state_.contracts) {
        if (c.assignee_faction_id != fid) continue;
        if (c.assigned_ship_id != ship_id) continue;
        if (c.status != ContractStatus::Accepted && c.status != ContractStatus::Offered) continue;

        // If the target is already complete/missing, drop the assignment and fall back.
        Id goal_sys = kInvalidId;
        Vec2 goal_pos{0.0, 0.0};
        if (!contract_goal(c, &goal_sys, &goal_pos)) {
          clear_contract_assignment(cid);
          break;
        }

        if (c.kind == ContractKind::BountyPirate && ship_weapons <= 1e-9) {
          // Don't assign bounties to unarmed ships.
          clear_contract_assignment(cid);
          break;
        }

        std::string err;
        if (assign_contract_to_ship(cid, ship_id, /*clear_existing_orders=*/false,
                                    /*restrict_to_discovered=*/true,
                                    /*push_event=*/false, &err)) {
          reserve_contract_target(c);
          return true;
        }

        // Could not issue; clear and fall back to exploration.
        clear_contract_assignment(cid);
        break;
      }

      // (1) Claim the best unassigned contract for this faction (AI may auto-accept).
      Id best_cid = kInvalidId;
      double best_score = -std::numeric_limits<double>::infinity();
      for (const auto& [cid, c] : state_.contracts) {
        if (c.assignee_faction_id != fid) continue;
        if (c.assigned_ship_id != kInvalidId || c.assigned_fleet_id != kInvalidId) continue;

        // Respect same-tick reservations from other auto behaviors.
        if (c.target_id != kInvalidId) {
          if (c.kind == ContractKind::InvestigateAnomaly && reserved_anoms.contains(c.target_id)) continue;
          if (c.kind == ContractKind::SalvageWreck && reserved_wrecks.contains(c.target_id)) continue;
          if (c.kind == ContractKind::SurveyJumpPoint && reserved_jumps.contains(c.target_id)) continue;
          if (c.kind == ContractKind::BountyPirate && reserved_bounties.contains(c.target_id)) continue;
        }

        const bool offered_ok = allow_auto_accept && c.status == ContractStatus::Offered;
        if (c.status != ContractStatus::Accepted && !offered_ok) continue;
        if (c.kind == ContractKind::BountyPirate && ship_weapons <= 1e-9) continue;

        Id goal_sys = kInvalidId;
        Vec2 goal_pos{0.0, 0.0};
        if (!contract_goal(c, &goal_sys, &goal_pos)) continue;

        const double eta = estimate_eta_days_to_pos(ship->system_id, ship->position_mkm, fid,
                                                    ship->speed_km_s, goal_sys, goal_pos);
        if (!std::isfinite(eta)) continue;

        double kind_mult = 1.0;
        if (c.kind == ContractKind::InvestigateAnomaly) kind_mult = 1.10;
        if (c.kind == ContractKind::SalvageWreck) kind_mult = 0.85;
        if (c.kind == ContractKind::BountyPirate) kind_mult = 0.95;

        const double rp = std::max(0.0, c.reward_research_points);
        const double score = kind_mult * (rp + 1.0) / (eta + 1.0) - c.risk_estimate * 0.25;
        if (best_cid == kInvalidId || score > best_score + 1e-9 ||
            (std::abs(score - best_score) <= 1e-9 && cid < best_cid)) {
          best_cid = cid;
          best_score = score;
        }
      }

      if (best_cid != kInvalidId) {
        std::string err;
        if (assign_contract_to_ship(best_cid, ship_id, /*clear_existing_orders=*/false,
                                    /*restrict_to_discovered=*/true,
                                    /*push_event=*/false, &err)) {
          if (const auto* c = find_ptr(state_.contracts, best_cid)) reserve_contract_target(*c);
          return true;
        }
      }
    }

    std::vector<Id> jps = sys->jump_points;
    std::sort(jps.begin(), jps.end());

    // (A) Prefer surveyed exits that are known to lead to undiscovered systems.
    Id best_jump = kInvalidId;
    double best_dist = std::numeric_limits<double>::infinity();
    for (Id jp_id : jps) {
      if (jp_id == kInvalidId) continue;
      if (reserved_jumps.contains(jp_id)) continue;

      if (cache && !cache->surveyed.contains(jp_id)) continue;

      const JumpPoint* jp = find_ptr(state_.jump_points, jp_id);
      if (!jp) continue;
      const JumpPoint* other = find_ptr(state_.jump_points, jp->linked_jump_id);
      if (!other) continue;

      const Id dest_sys = other->system_id;
      if (dest_sys == kInvalidId) continue;

      const bool dest_known = cache ? cache->discovered.contains(dest_sys)
                                    : is_system_discovered_by_faction(fid, dest_sys);
      if (dest_known) continue;

      const double dist = (ship->position_mkm - jp->position_mkm).length();
      if (best_jump == kInvalidId || dist + 1e-9 < best_dist ||
          (std::abs(dist - best_dist) <= 1e-9 && jp_id < best_jump)) {
        best_jump = jp_id;
        best_dist = dist;
      }
    }

    if (best_jump != kInvalidId) {
      reserved_jumps.insert(best_jump);
      issue_travel_via_jump(ship_id, best_jump);
      return true;
    }

    // (B) Survey unknown exits (move to the jump point, but do NOT automatically transit).
    Id best_survey = kInvalidId;
    double best_survey_dist = std::numeric_limits<double>::infinity();
    Vec2 best_survey_pos = Vec2{};
    for (Id jp_id : jps) {
      if (jp_id == kInvalidId) continue;
      if (reserved_jumps.contains(jp_id)) continue;

      const bool surveyed = cache ? cache->surveyed.contains(jp_id) : is_jump_point_surveyed_by_faction(fid, jp_id);
      if (surveyed) continue;

      const JumpPoint* jp = find_ptr(state_.jump_points, jp_id);
      if (!jp) continue;

      const double dist = (ship->position_mkm - jp->position_mkm).length();
      if (best_survey == kInvalidId || dist + 1e-9 < best_survey_dist ||
          (std::abs(dist - best_survey_dist) <= 1e-9 && jp_id < best_survey)) {
        best_survey = jp_id;
        best_survey_dist = dist;
        best_survey_pos = jp->position_mkm;
      }
    }

    if (best_survey != kInvalidId) {
      reserved_jumps.insert(best_survey);
      // If this exit leads to an undiscovered system, prefer surveying and immediately transiting
      // to reduce idle re-planning churn.
      bool transit_when_done = false;
      if (const JumpPoint* jp = find_ptr(state_.jump_points, best_survey)) {
        if (const JumpPoint* other = find_ptr(state_.jump_points, jp->linked_jump_id)) {
          const Id dest_sys = other->system_id;
          if (dest_sys != kInvalidId) {
            const bool dest_known = cache ? cache->discovered.contains(dest_sys)
                                          : is_system_discovered_by_faction(fid, dest_sys);
            transit_when_done = !dest_known;
          }
        }
      }

      issue_survey_jump_point(ship_id, best_survey, transit_when_done, /*restrict_to_discovered=*/true);
      return true;
    }

    // (C) No work here. Route to the best frontier system.
    if (!cache) return false;

    Id best_frontier = kInvalidId;
    double best_score = -std::numeric_limits<double>::infinity();

    for (const auto& fr : cache->frontiers) {
      const Id sys_id = fr.system_id;
      if (sys_id == kInvalidId) continue;
      if (sys_id == ship->system_id) continue;
      if (reserved_frontiers.contains(sys_id)) continue;

      const double eta = estimate_eta_days_to_system(ship->system_id, ship->position_mkm, fid, ship->speed_km_s, sys_id);
      if (!std::isfinite(eta)) continue;

      // Score: more frontier work is better; ETA is worse.
      const double score = static_cast<double>(fr.weight()) * 1000.0 - eta * 10.0;

      if (best_frontier == kInvalidId || score > best_score + 1e-9 ||
          (std::abs(score - best_score) <= 1e-9 && sys_id < best_frontier)) {
        best_frontier = sys_id;
        best_score = score;
      }
    }

    if (best_frontier != kInvalidId) {
      reserved_frontiers.insert(best_frontier);
      return issue_travel_to_system(ship_id, best_frontier, /*restrict_to_discovered=*/true);
    }

    // (D) Fully explored: investigate unresolved anomalies in the current system.
    {
      const ShipDesign* d = find_design(ship->design_id);
      const double speed_mkm_d = (d && d->speed_km_s > 1e-9) ? mkm_per_day_from_speed(d->speed_km_s, cfg_.seconds_per_day) : 1.0;

      Id best_anom = kInvalidId;
      double best_anom_score = -std::numeric_limits<double>::infinity();
      double best_d2 = std::numeric_limits<double>::infinity();

      for (const auto& [aid, a] : state_.anomalies) {
        if (aid == kInvalidId) continue;
        if (a.system_id != ship->system_id) continue;
        if (a.resolved) continue;
        if (!is_anomaly_discovered_by_faction(fid, aid)) continue;
        if (reserved_anoms.contains(aid)) continue;

        double minerals_total = 0.0;
        for (const auto& [_, t] : a.mineral_reward) minerals_total += std::max(0.0, t);

        double value = std::max(0.0, a.research_reward);
        value += minerals_total * 0.05; // heuristic: 20t ~ 1 RP
        if (!a.unlock_component_id.empty()) value += 25.0;

        const double risk = std::clamp(a.hazard_chance, 0.0, 1.0) * std::max(0.0, a.hazard_damage);

        const double d2 = (ship->position_mkm - a.position_mkm).length_squared();
        const double dist = std::sqrt(std::max(0.0, d2));
        const double travel_days = dist / std::max(1e-6, speed_mkm_d);

        // Prefer high-value, low-risk anomalies; discount by travel time within the system.
        const double score = value / (1.0 + travel_days) - risk;

        if (best_anom == kInvalidId || score > best_anom_score + 1e-9 ||
            (std::abs(score - best_anom_score) <= 1e-9 &&
             (d2 + 1e-9 < best_d2 || (std::abs(d2 - best_d2) <= 1e-9 && aid < best_anom)))) {
          best_anom = aid;
          best_anom_score = score;
          best_d2 = d2;
        }
      }

      if (best_anom != kInvalidId) {
        reserved_anoms.insert(best_anom);
        return issue_investigate_anomaly(ship_id, best_anom, /*restrict_to_discovered=*/true);
      }
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

  // --- Ship-level automation: Auto-rearm (munition safety) ---
  for (Id sid : ship_ids) {
    Ship* sh = find_ptr(state_.ships, sid);
    if (!sh) continue;
    if (!sh->auto_rearm) continue;
    if (!orders_empty(sid)) continue;

    (void)issue_auto_rearm(sid);
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

  // Implementation note:
  // Use the shared Fuel Planner so UI previews and automation remain consistent.
  {
    FuelPlannerOptions opt;
    opt.require_auto_tanker_flag = true;
    opt.require_idle = true;
    opt.restrict_to_discovered = true;
    opt.exclude_fleet_ships = true;
    opt.exclude_ships_with_auto_refuel = true;

    // Keep legacy behavior: one dispatch per idle tanker. (Multi-stop routes can be
    // generated/applied from the Fuel Planner UI.)
    opt.max_legs_per_tanker = 1;

    // Safety caps (large enough to not break typical automation in bigger saves).
    opt.max_targets = 4096;
    opt.max_tankers = 4096;

    for (Id fid : faction_ids) {
      const auto plan = compute_fuel_plan(*this, fid, opt);
      if (!plan.ok || plan.assignments.empty()) continue;
      (void)apply_fuel_plan(*this, plan, /*clear_existing_orders=*/false);
    }
  }


  // --- Ship-level automation: Auto-troop transport (garrison logistics) ---

  // Implementation note:
  // Use the shared Troop Planner so UI previews and automation remain consistent.
  {
    TroopPlannerOptions opt;
    opt.require_auto_troop_transport_flag = true;
    opt.require_idle = true;
    opt.restrict_to_discovered = true;
    opt.exclude_fleet_ships = true;

    // Safety cap (large enough to not break typical automation in bigger saves).
    opt.max_ships = 4096;

    for (Id fid : faction_ids) {
      const auto plan = compute_troop_plan(*this, fid, opt);
      if (!plan.ok || plan.assignments.empty()) continue;
      (void)apply_troop_plan(*this, plan, /*clear_existing_orders=*/false);
    }
  }

  // --- Ship-level automation: Auto-colonist transport (population logistics) ---

  // Implementation note:
  // Use the shared Colonist Planner so UI previews and automation remain consistent.
  {
    ColonistPlannerOptions opt;
    opt.require_auto_colonist_transport_flag = true;
    opt.require_idle = true;
    opt.restrict_to_discovered = true;
    opt.exclude_fleet_ships = true;

    // Safety cap (large enough to not break typical automation in bigger saves).
    opt.max_ships = 4096;

    for (Id fid : faction_ids) {
      const auto plan = compute_colonist_plan(*this, fid, opt);
      if (!plan.ok || plan.assignments.empty()) continue;
      (void)apply_colonist_plan(*this, plan, /*clear_existing_orders=*/false);
    }
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
      } else if (const auto* sl = std::get_if<SalvageWreckLoop>(&ord)) {
        if (sl->wreck_id != kInvalidId) reserved_wreck_targets[ship->faction_id].insert(sl->wreck_id);
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
      return static_cast<double>(stable_sum_nonneg_sorted_ld(s.cargo));
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
    if (sh->auto_mine) continue;      // mutually exclusive; auto-mine handled below
    if (sh->auto_colonize) continue;  // mutually exclusive; auto-colonize handled below
    if (sh->auto_tanker) continue;    // mutually exclusive; auto-tanker handled above
    if (!orders_empty(sid)) continue;

    (void)issue_auto_salvage(sid);
  }

  // --- Ship-level automation: Auto-mine (mobile mining) ---
  // Reserve body targets that are already being mined (or en-route) so we don't
  // send multiple automated miners to the same body.
  std::unordered_map<Id, std::unordered_set<Id>> reserved_mine_targets;
  reserved_mine_targets.reserve(faction_ids.size() * 2 + 4);
  for (const auto& [sid, so] : state_.ship_orders) {
    const Ship* ship = find_ptr(state_.ships, sid);
    if (!ship) continue;
    if (ship->faction_id == kInvalidId) continue;
    for (const auto& ord : so.queue) {
      if (const auto* mb = std::get_if<MineBody>(&ord)) {
        if (mb->body_id != kInvalidId) reserved_mine_targets[ship->faction_id].insert(mb->body_id);
      }
    }
  }

  const auto body_ids = sorted_keys(state_.bodies);

  auto issue_auto_mine = [&](Id ship_id) -> bool {
    Ship* ship = find_ptr(state_.ships, ship_id);
    if (!ship) return false;
    if (!ship->auto_mine) return false;
    if (!orders_empty(ship_id)) return false;
    if (ship->system_id == kInvalidId) return false;
    if (ship->speed_km_s <= 0.0) return false;

    // Avoid fighting the fleet movement logic. Fleets should be controlled by fleet orders.
    if (fleet_for_ship(ship_id) != kInvalidId) return false;

    const ShipDesign* d = find_design(ship->design_id);
    const double cap = d ? std::max(0.0, d->cargo_tons) : 0.0;
    const double mine_rate = d ? std::max(0.0, d->mining_tons_per_day) : 0.0;
    if (cap <= 1e-9 || mine_rate <= 1e-9) return false;

    auto cargo_used_tons = [&](const Ship& s) {
      return static_cast<double>(stable_sum_nonneg_sorted_ld(s.cargo));
    };

    const double used = cargo_used_tons(*ship);

    // 1) If we're carrying anything, deliver it to the configured home colony (if valid),
    //    otherwise deliver to the nearest friendly colony.
    if (used > 1e-6) {
      Id best_colony_id = kInvalidId;
      double best_eta = std::numeric_limits<double>::infinity();

      const auto try_colony = [&](Id cid) {
        const Colony* c = find_ptr(state_.colonies, cid);
        if (!c) return;
        if (c->faction_id != ship->faction_id) return;
        const Body* b = find_ptr(state_.bodies, c->body_id);
        if (!b) return;
        if (b->system_id == kInvalidId) return;
        const double eta = estimate_eta_days_to_pos(ship->system_id, ship->position_mkm, ship->faction_id,
                                                    ship->speed_km_s, b->system_id, b->position_mkm);
        if (!std::isfinite(eta)) return;
        if (eta < best_eta) {
          best_eta = eta;
          best_colony_id = cid;
        }
      };

      if (ship->auto_mine_home_colony_id != kInvalidId) {
        try_colony(ship->auto_mine_home_colony_id);
      }
      if (best_colony_id == kInvalidId) {
        for (Id cid : sorted_keys(state_.colonies)) try_colony(cid);
      }
      if (best_colony_id == kInvalidId) return false;

      return issue_unload_mineral(ship_id, best_colony_id, /*mineral=*/"", /*tons=*/0.0,
                                 /*restrict_to_discovered=*/true);
    }

    // 2) Otherwise, find the best available deposit in discovered space.
    const Id fid = ship->faction_id;
    auto& reserved = reserved_mine_targets[fid];
    const std::string want = ship->auto_mine_mineral;

    Id best_body_id = kInvalidId;
    double best_score = -std::numeric_limits<double>::infinity();
    double best_eta = std::numeric_limits<double>::infinity();
    double best_deposit = 0.0;

    for (Id bid : body_ids) {
      const Body* b = find_ptr(state_.bodies, bid);
      if (!b) continue;
      if (b->system_id == kInvalidId) continue;
      if (!find_ptr(state_.systems, b->system_id)) continue;

      // Honor fog-of-war.
      if (!is_system_discovered_by_faction(fid, b->system_id)) continue;

      // Skip unmineable body types.
      if (b->type == BodyType::Star) continue;

      if (reserved.find(bid) != reserved.end()) continue;

      double deposit = 0.0;
      if (b->mineral_deposits.empty()) {
        // Legacy/unmodeled: treat as effectively infinite so players can keep using older saves.
        deposit = 1.0e12;
      } else if (!want.empty()) {
        // Modeled deposits: missing keys mean absent.
        auto it = b->mineral_deposits.find(want);
        deposit = (it == b->mineral_deposits.end()) ? 0.0 : std::max(0.0, it->second);
      } else {
        // Sum all remaining deposits.
        for (const auto& [_, tons] : b->mineral_deposits) deposit += std::max(0.0, tons);
      }

      // Avoid depleted deposits.
      if (deposit <= 1e-6) continue;

      const double eta = estimate_eta_days_to_pos(ship->system_id, ship->position_mkm, fid, ship->speed_km_s,
                                                  b->system_id, b->position_mkm);
      if (!std::isfinite(eta)) continue;

      // Score: prefer big deposits, prefer nearer targets.
      double score = std::log10(deposit + 1.0) * 100.0 - eta;
      // Gentle bias toward asteroids/comets as "intended" mobile mining targets.
      if (b->type == BodyType::Asteroid) score += 10.0;
      if (b->type == BodyType::Comet) score += 8.0;

      if (score > best_score + 1e-9 ||
          (std::abs(score - best_score) <= 1e-9 && (eta < best_eta - 1e-9 ||
                                                   (std::abs(eta - best_eta) <= 1e-9 && deposit > best_deposit + 1e-9)))) {
        best_score = score;
        best_body_id = bid;
        best_eta = eta;
        best_deposit = deposit;
      }
    }

    if (best_body_id == kInvalidId) return false;
    reserved.insert(best_body_id);
    return issue_mine_body(ship_id, best_body_id, want, /*stop_when_cargo_full=*/true,
                           /*restrict_to_discovered=*/true);
  };

  for (Id sid : ship_ids) {
    Ship* sh = find_ptr(state_.ships, sid);
    if (!sh) continue;
    if (!sh->auto_mine) continue;
    if (sh->auto_explore) continue;
    if (sh->auto_freight) continue;
    if (sh->auto_salvage) continue;
    if (sh->auto_colonize) continue;
    if (sh->auto_tanker) continue;
    if (!orders_empty(sid)) continue;

    (void)issue_auto_mine(sid);
  }

  // --- Ship-level automation: Auto-colonize ---
  for (Id sid : ship_ids) {
    Ship* sh = find_ptr(state_.ships, sid);
    if (!sh) continue;
    if (!sh->auto_colonize) continue;
    if (sh->auto_explore) continue;   // mutually exclusive; auto-explore handled below
    if (sh->auto_freight) continue;   // mutually exclusive; auto-freight handled below
    if (sh->auto_salvage) continue;   // mutually exclusive; auto-salvage handled above
    if (sh->auto_mine) continue;      // mutually exclusive; auto-mine handled above
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
    if (sh->auto_mine) continue;     // mutually exclusive; auto-mine handled above
    if (sh->auto_tanker) continue;   // mutually exclusive; auto-tanker handled above
    if (!orders_empty(sid)) continue;

    (void)issue_auto_explore(sid);
  }


  // --- Ship-level automation: Auto-freight (mineral logistics) ---
  {
    NEBULA4X_TRACE_SCOPE("tick_auto_freight", "sim.ai");

    // Note: auto_freight_min_transfer_tons can be configured to 0 in some saves.
    // Guard against degenerate 0-ton plans by clamping to a small epsilon.
    const double min_tons = std::max(1e-6, cfg_.auto_freight_min_transfer_tons);

    // Collect factions that have eligible idle auto-freight ships.
    std::unordered_map<Id, int> eligible_count;
    eligible_count.reserve(faction_ids.size() * 2);

    for (Id sid : ship_ids) {
      Ship* sh = find_ptr(state_.ships, sid);
      if (!sh) continue;
      if (!sh->auto_freight) continue;
      if (sh->auto_explore) continue;   // mutually exclusive; auto-explore handled above
      if (sh->auto_colonize) continue;  // mutually exclusive; auto-colonize handled above
      if (sh->auto_salvage) continue;   // mutually exclusive; auto-salvage handled above
      if (sh->auto_mine) continue;      // mutually exclusive; auto-mine handled above
      if (sh->auto_tanker) continue;    // mutually exclusive; auto-tanker handled above
      if (!orders_empty(sid)) continue;
      if (sh->system_id == kInvalidId) continue;
      if (sh->speed_km_s <= 0.0) continue;

      // Avoid fighting the fleet movement logic. Fleets should be controlled by fleet orders.
      if (fleet_for_ship(sid) != kInvalidId) continue;

      const auto* d = find_design(sh->design_id);
      const double cap = d ? std::max(0.0, d->cargo_tons) : 0.0;
      if (cap < min_tons) continue;

      eligible_count[sh->faction_id]++;
    }

    if (!eligible_count.empty()) {
      std::vector<Id> fids;
      fids.reserve(eligible_count.size());
      for (const auto& kv : eligible_count) fids.push_back(kv.first);
      std::sort(fids.begin(), fids.end());

      // Plan/apply per faction so we coordinate supply (avoid multiple ships "double counting" the same exportable minerals).
      for (Id fid : fids) {
        FreightPlannerOptions opt;
        opt.require_auto_freight_flag = true;
        opt.require_idle = true;
        opt.restrict_to_discovered = true;
        opt.max_ships = std::clamp(eligible_count[fid], 1, 4096);

        const FreightPlannerResult plan = compute_freight_plan(*this, fid, opt);
        if (!plan.ok || plan.assignments.empty()) continue;

        // Idle ships have empty queues, but we still clear defensively to keep behavior consistent.
        (void)apply_freight_plan(*this, plan, /*clear_existing_orders=*/true);
      }
    }
  }


// --- AI empire fleet organization (non-player automation) ---
  //
  // Random scenarios can spawn multiple AI-controlled empires.
  // Economic AI keeps their colonies and shipyards progressing, but their
  // combat ships were previously left idle because fleet missions were
  // player-only and AI did not form/assign mission fleets.
  //
  // This block creates a small, stable set of fleets for each AI empire and
  // assigns them missions:
  //   - Defense Fleet: Defend capital colony (system-wide by default)
  //   - Escort Fleet: Escort auto-freight traffic
  //   - Patrol Fleet: Patrol the capital region/system (also contributes to
  //                   piracy suppression)
  {
    NEBULA4X_TRACE_SCOPE("tick_ai_empire_fleets", "sim.ai");

    const int now_day = static_cast<int>(state_.date.days_since_epoch());

    auto capital_colony_for_faction = [&](Id fid) -> Id {
      Id best_cid = kInvalidId;
      double best_pop = -1.0;
      for (Id cid : sorted_keys(state_.colonies)) {
        const Colony* c = find_ptr(state_.colonies, cid);
        if (!c || c->faction_id != fid) continue;
        const double pop = std::max(0.0, c->population_millions);
        if (best_cid == kInvalidId || pop > best_pop + 1e-9 ||
            (std::abs(pop - best_pop) <= 1e-9 && cid < best_cid)) {
          best_cid = cid;
          best_pop = pop;
        }
      }
      return best_cid;
    };

    auto ship_role_of = [&](Id sid) -> ShipRole {
      const Ship* sh = find_ptr(state_.ships, sid);
      if (!sh) return ShipRole::Unknown;
      const ShipDesign* d = find_design(sh->design_id);
      return d ? d->role : ShipRole::Unknown;
    };

    auto ensure_fleet_mission_defaults = [&](Fleet& fl) {
      // Keep AI fleets a bit more conservative than the UI defaults.
      fl.mission.auto_refuel = true;
      fl.mission.refuel_threshold_fraction = std::clamp(fl.mission.refuel_threshold_fraction, 0.30, 0.60);
      fl.mission.refuel_resume_fraction = std::clamp(fl.mission.refuel_resume_fraction, 0.80, 0.98);

      fl.mission.auto_repair = true;
      fl.mission.repair_threshold_fraction = std::clamp(fl.mission.repair_threshold_fraction, 0.55, 0.85);
      fl.mission.repair_resume_fraction = std::clamp(fl.mission.repair_resume_fraction, 0.85, 0.99);

      fl.mission.auto_rearm = true;
      fl.mission.rearm_threshold_fraction = std::clamp(fl.mission.rearm_threshold_fraction, 0.30, 0.60);
      fl.mission.rearm_resume_fraction = std::clamp(fl.mission.rearm_resume_fraction, 0.80, 0.98);

      fl.mission.auto_maintenance = true;
      fl.mission.maintenance_threshold_fraction = std::clamp(fl.mission.maintenance_threshold_fraction, 0.70, 0.90);
      fl.mission.maintenance_resume_fraction = std::clamp(fl.mission.maintenance_resume_fraction, 0.90, 0.99);
    };

    const auto fleet_ids_snapshot = sorted_keys(state_.fleets);

    // Lazy cache for trade-security patrol scoring (computed only if enabled).
    bool trade_security_cache_ready = false;
    TradeNetwork trade_security_net;
    std::unordered_map<Id, Vec2> trade_security_hub_pos;
    std::unordered_map<Id, double> trade_security_hub_pop;
    bool trade_security_hubs_ready = false;

    auto ensure_trade_security_cache = [&]() {
      if (trade_security_cache_ready) return;
      TradeNetworkOptions topt;
      topt.max_lanes = std::max(1, cfg_.ai_trade_security_patrol_consider_top_lanes);
      topt.include_uncolonized_markets = false;
      topt.include_colony_contributions = true;
      trade_security_net = compute_trade_network(*this, topt);
      trade_security_cache_ready = true;
    };

    auto ensure_trade_security_hubs = [&]() {
      if (trade_security_hubs_ready) return;
      trade_security_hub_pos.clear();
      trade_security_hub_pop.clear();
      trade_security_hub_pos.reserve(state_.colonies.size() + 8);
      trade_security_hub_pop.reserve(state_.colonies.size() + 8);
      for (const auto& [cid, c] : state_.colonies) {
        (void)cid;
        const Body* b = find_ptr(state_.bodies, c.body_id);
        if (!b) continue;
        if (b->system_id == kInvalidId) continue;
        const double pop = std::max(0.0, c.population_millions);
        auto itp = trade_security_hub_pop.find(b->system_id);
        if (itp == trade_security_hub_pop.end() || pop > itp->second + 1e-9) {
          trade_security_hub_pop[b->system_id] = pop;
          trade_security_hub_pos[b->system_id] = b->position_mkm;
        }
      }
      trade_security_hubs_ready = true;
    };


    for (Id fid : faction_ids) {
      Faction* fac = find_ptr(state_.factions, fid);
      if (!fac) continue;
      if (fac->control != FactionControl::AI_Explorer) continue;

      const Id capital_colony = capital_colony_for_faction(fid);
      if (capital_colony == kInvalidId) continue;

      const Colony* capc = find_ptr(state_.colonies, capital_colony);
      if (!capc) continue;
      const Body* capb = find_ptr(state_.bodies, capc->body_id);
      const StarSystem* caps = capb ? find_ptr(state_.systems, capb->system_id) : nullptr;

      const Id capital_sys = capb ? capb->system_id : kInvalidId;
      const Id capital_region = caps ? caps->region_id : kInvalidId;

      // Existing fleets and membership.
      Fleet* defense_fleet = nullptr;
      Fleet* escort_fleet = nullptr;
      Fleet* patrol_fleet = nullptr;

      std::unordered_set<Id> ships_in_fleets;
      ships_in_fleets.reserve(64);

      for (Id flid : fleet_ids_snapshot) {
        Fleet* fl = find_ptr(state_.fleets, flid);
        if (!fl || fl->faction_id != fid) continue;

        for (Id sid : fl->ship_ids) ships_in_fleets.insert(sid);

        if (!defense_fleet && fl->mission.type == FleetMissionType::DefendColony &&
            fl->mission.defend_colony_id == capital_colony) {
          defense_fleet = fl;
          continue;
        }
        if (!escort_fleet && fl->mission.type == FleetMissionType::EscortFreighters) {
          escort_fleet = fl;
          continue;
        }
        if (!patrol_fleet &&
            (fl->mission.type == FleetMissionType::PatrolRegion || fl->mission.type == FleetMissionType::PatrolSystem)) {
          patrol_fleet = fl;
          continue;
        }
      }

      // Do we have auto-freight traffic worth escorting?
      bool has_auto_freight = false;
      for (Id sid : ship_ids) {
        const Ship* sh = find_ptr(state_.ships, sid);
        if (!sh || sh->faction_id != fid) continue;
        if (ship_role_of(sid) != ShipRole::Freighter) continue;
        if (!sh->auto_freight) continue;
        has_auto_freight = true;
        break;
      }

      // Gather unassigned combatants (sorted) for deterministic assignment.
      std::vector<Id> unassigned_combatants;
      for (Id sid : ship_ids) {
        const Ship* sh = find_ptr(state_.ships, sid);
        if (!sh) continue;
        if (sh->faction_id != fid) continue;
        if (ships_in_fleets.count(sid)) continue;

        if (ship_role_of(sid) != ShipRole::Combatant) continue;

        // Skip immobile bases/stations.
        if (sh->speed_km_s <= 0.0) continue;

        unassigned_combatants.push_back(sid);
      }

      std::size_t take_idx = 0;
      auto take_next = [&]() -> Id {
        if (take_idx >= unassigned_combatants.size()) return kInvalidId;
        return unassigned_combatants[take_idx++];
      };

      auto take_group = [&](int n) -> std::vector<Id> {
        std::vector<Id> out;
        out.reserve(static_cast<std::size_t>(std::max(0, n)));
        for (int i = 0; i < n; ++i) {
          const Id sid = take_next();
          if (sid == kInvalidId) break;
          out.push_back(sid);
        }
        return out;
      };

      auto fill_fleet_to = [&](Fleet* fl, int target_size) {
        if (!fl) return;
        while (static_cast<int>(fl->ship_ids.size()) < target_size) {
          const Id sid = take_next();
          if (sid == kInvalidId) break;
          std::string err;
          (void)add_ship_to_fleet(fl->id, sid, &err);
        }
      };

      // Create/maintain fleets in a stable order.
      // 1) Defense
      if (!defense_fleet) {
        auto group = take_group(2);
        if (!group.empty()) {
          std::string err;
          const Id nfl = create_fleet(fid, "Defense Fleet", group, &err);
          defense_fleet = find_ptr(state_.fleets, nfl);
        }
      }
      if (defense_fleet) {
        defense_fleet->mission.type = FleetMissionType::DefendColony;
        defense_fleet->mission.defend_colony_id = capital_colony;
        defense_fleet->mission.defend_radius_mkm = 0.0;
        ensure_fleet_mission_defaults(*defense_fleet);
        (void)configure_fleet_formation(defense_fleet->id, FleetFormation::Wedge, 2.0);
        fill_fleet_to(defense_fleet, 2);
      }

      // 2) Escort
      if (has_auto_freight) {
        if (!escort_fleet) {
          auto group = take_group(1);
          if (!group.empty()) {
            std::string err;
            const Id nfl = create_fleet(fid, "Escort Fleet", group, &err);
            escort_fleet = find_ptr(state_.fleets, nfl);
          }
        }
        if (escort_fleet) {
          escort_fleet->mission.type = FleetMissionType::EscortFreighters;
          escort_fleet->mission.escort_target_ship_id = kInvalidId;
          escort_fleet->mission.escort_only_auto_freight = true;
          escort_fleet->mission.escort_follow_distance_mkm = 2.0;
          escort_fleet->mission.escort_defense_radius_mkm = 0.0; // in-system
          ensure_fleet_mission_defaults(*escort_fleet);
          (void)configure_fleet_formation(escort_fleet->id, FleetFormation::Column, 2.0);
          fill_fleet_to(escort_fleet, 1);
        }
      }

      // 3) Patrol (uses all remaining combatants)
      if (!patrol_fleet) {
        std::vector<Id> group;
        while (true) {
          const Id sid = take_next();
          if (sid == kInvalidId) break;
          group.push_back(sid);
        }
        if (!group.empty()) {
          std::string err;
          const Id nfl = create_fleet(fid, "Patrol Fleet", group, &err);
          patrol_fleet = find_ptr(state_.fleets, nfl);
        }
      }
      if (patrol_fleet) {
        auto set_patrol_target_capital = [&]() {
          if (capital_region != kInvalidId) {
            patrol_fleet->mission.type = FleetMissionType::PatrolRegion;
            patrol_fleet->mission.patrol_region_id = capital_region;
            patrol_fleet->mission.patrol_region_dwell_days = 4;
            patrol_fleet->mission.patrol_region_system_index = 0;
            patrol_fleet->mission.patrol_region_waypoint_index = 0;
          } else {
            patrol_fleet->mission.type = FleetMissionType::PatrolSystem;
            patrol_fleet->mission.patrol_system_id = capital_sys;
            patrol_fleet->mission.patrol_dwell_days = 4;
            patrol_fleet->mission.patrol_leg_index = 0;
          }
        };

        const bool mission_is_patrol =
            patrol_fleet->mission.type == FleetMissionType::PatrolRegion ||
            patrol_fleet->mission.type == FleetMissionType::PatrolSystem;

        // Fresh fleets start with a sensible capital patrol mission.
        if (!mission_is_patrol) {
          set_patrol_target_capital();
        } else {
          // Validate target ids (protect against partially-initialized saves).
          if (patrol_fleet->mission.type == FleetMissionType::PatrolRegion &&
              patrol_fleet->mission.patrol_region_id == kInvalidId &&
              capital_region != kInvalidId) {
            set_patrol_target_capital();
          }
          if (patrol_fleet->mission.type == FleetMissionType::PatrolSystem &&
              patrol_fleet->mission.patrol_system_id == kInvalidId) {
            set_patrol_target_capital();
          }
        }

        // Trade-security retasking: choose patrol regions procedurally from the
        // current trade network and piracy risk map.
        if (cfg_.enable_ai_trade_security_patrols) {
          int interval = cfg_.ai_trade_security_patrol_retarget_interval_days;
          if (interval <= 0) interval = 1;
          const bool due =
              interval <= 1 ||
              (((now_day + static_cast<int>(patrol_fleet->id)) % interval) == 0);

          if (due) {
            // Systems containing our colonies represent direct economic exposure.
            std::unordered_set<Id> own_colony_systems;
            own_colony_systems.reserve(16);
            for (const auto& [cid, c] : state_.colonies) {
              (void)cid;
              if (c.faction_id != fid) continue;
              const Body* b = find_ptr(state_.bodies, c.body_id);
              if (!b) continue;
              if (b->system_id == kInvalidId) continue;
              own_colony_systems.insert(b->system_id);
            }

            if (!own_colony_systems.empty()) {
              ensure_trade_security_cache();
              ensure_trade_security_hubs();

              // Score systems by trade throughput (volume share), amplified by
              // effective piracy risk and our own colony presence.
              std::unordered_map<Id, double> need_by_system;
              need_by_system.reserve(64);

              const double min_lane_vol = std::max(0.0, cfg_.ai_trade_security_patrol_min_lane_volume);
              const double risk_w = std::max(0.0, cfg_.ai_trade_security_patrol_risk_weight);
              const double own_w = std::max(1.0, cfg_.ai_trade_security_patrol_own_colony_weight);

              for (const auto& lane : trade_security_net.lanes) {
                if (!(lane.total_volume > min_lane_vol)) continue;
                if (lane.from_system_id == kInvalidId || lane.to_system_id == kInvalidId) continue;
                if (lane.from_system_id == lane.to_system_id) continue;

                const bool relevant =
                    own_colony_systems.contains(lane.from_system_id) ||
                    own_colony_systems.contains(lane.to_system_id);
                if (!relevant) continue;

                Vec2 start_pos_mkm{0.0, 0.0};
                if (auto it = trade_security_hub_pos.find(lane.from_system_id); it != trade_security_hub_pos.end()) {
                  start_pos_mkm = it->second;
                }
                std::optional<Vec2> goal_pos_mkm;
                if (auto it = trade_security_hub_pos.find(lane.to_system_id); it != trade_security_hub_pos.end()) {
                  goal_pos_mkm = it->second;
                }

                // Restrict to what the faction can actually navigate.
                const auto plan = plan_jump_route_cached(
                    lane.from_system_id, start_pos_mkm, fid,
                    /*speed_km_s=*/1000.0, lane.to_system_id,
                    /*restrict_to_discovered=*/true, goal_pos_mkm);
                if (!plan) continue;
                if (plan->systems.empty()) continue;

                const double vol_share = lane.total_volume / static_cast<double>(plan->systems.size());
                for (Id sys_id : plan->systems) {
                  if (sys_id == kInvalidId) continue;
                  const double risk = piracy_risk_for_system(sys_id);
                  double need = vol_share * (0.20 + risk_w * risk);
                  if (own_colony_systems.contains(sys_id)) need *= own_w;
                  need_by_system[sys_id] += need;
                }
              }

              // Collect discovered regions for filtering.
              std::unordered_set<Id> discovered_regions;
              if (const Faction* f = find_ptr(state_.factions, fid)) {
                discovered_regions.reserve(f->discovered_systems.size() * 2 + 8);
                for (Id sys_id : f->discovered_systems) {
                  const StarSystem* sys = find_ptr(state_.systems, sys_id);
                  if (!sys) continue;
                  if (sys->region_id != kInvalidId) discovered_regions.insert(sys->region_id);
                }
              }
              if (capital_region != kInvalidId) discovered_regions.insert(capital_region);

              // Reduce to region scores (and keep a representative system).
              std::unordered_map<Id, double> need_by_region;
              std::unordered_map<Id, Id> best_sys_for_region;
              std::unordered_map<Id, double> best_sys_need;
              std::unordered_map<Id, double> need_no_region;

              for (const auto& [sys_id, need] : need_by_system) {
                const StarSystem* sys = find_ptr(state_.systems, sys_id);
                if (!sys) continue;
                if (!is_system_discovered_by_faction(fid, sys_id)) continue;

                const Id rid = sys->region_id;
                if (rid != kInvalidId) {
                  need_by_region[rid] += need;

                  auto itn = best_sys_need.find(rid);
                  if (itn == best_sys_need.end() || need > itn->second + 1e-9 ||
                      (std::abs(need - itn->second) <= 1e-9 && sys_id < best_sys_for_region[rid])) {
                    best_sys_need[rid] = need;
                    best_sys_for_region[rid] = sys_id;
                  }
                } else {
                  need_no_region[sys_id] += need;
                }
              }

              // Estimate travel cost from the capital to discourage cross-sector ping-pong.
              double patrol_speed = std::numeric_limits<double>::infinity();
              for (Id sid : patrol_fleet->ship_ids) {
                const Ship* sh = find_ptr(state_.ships, sid);
                if (!sh) continue;
                const double sp = std::max(0.0, sh->speed_km_s);
                if (sp > 1e-6) patrol_speed = std::min(patrol_speed, sp);
              }
              if (!std::isfinite(patrol_speed) || patrol_speed <= 1e-6) patrol_speed = 1000.0;

              Vec2 cap_pos_mkm{0.0, 0.0};
              if (capb) cap_pos_mkm = capb->position_mkm;

              auto eta_penalized_score = [&](double need, Id target_sys) -> double {
                if (!(need > 0.0)) return -std::numeric_limits<double>::infinity();
                if (target_sys == kInvalidId) return -std::numeric_limits<double>::infinity();
                const double eta = estimate_eta_days_to_system(
                    capital_sys, cap_pos_mkm, fid, patrol_speed, target_sys);
                if (!std::isfinite(eta)) return -std::numeric_limits<double>::infinity();
                return need / (1.0 + eta * 0.05);
              };

              Id best_region = kInvalidId;
              double best_region_score = -std::numeric_limits<double>::infinity();
              for (const auto& [rid, need] : need_by_region) {
                if (!discovered_regions.contains(rid)) continue;
                Id target_sys = kInvalidId;
                if (auto it = best_sys_for_region.find(rid); it != best_sys_for_region.end()) {
                  target_sys = it->second;
                }
                const double score = eta_penalized_score(need, target_sys);
                if (score > best_region_score + 1e-9 ||
                    (std::abs(score - best_region_score) <= 1e-9 && rid < best_region)) {
                  best_region_score = score;
                  best_region = rid;
                }
              }

              Id best_system = kInvalidId;
              double best_system_score = -std::numeric_limits<double>::infinity();
              for (const auto& [sys_id, need] : need_no_region) {
                const double score = eta_penalized_score(need, sys_id);
                if (score > best_system_score + 1e-9 ||
                    (std::abs(score - best_system_score) <= 1e-9 && sys_id < best_system)) {
                  best_system_score = score;
                  best_system = sys_id;
                }
              }

              if (best_region != kInvalidId) {
                if (patrol_fleet->mission.type != FleetMissionType::PatrolRegion ||
                    patrol_fleet->mission.patrol_region_id != best_region) {
                  patrol_fleet->mission.type = FleetMissionType::PatrolRegion;
                  patrol_fleet->mission.patrol_region_id = best_region;
                  patrol_fleet->mission.patrol_region_dwell_days = 4;
                  patrol_fleet->mission.patrol_region_system_index = 0;
                  patrol_fleet->mission.patrol_region_waypoint_index = 0;
                }
              } else if (best_system != kInvalidId) {
                if (patrol_fleet->mission.type != FleetMissionType::PatrolSystem ||
                    patrol_fleet->mission.patrol_system_id != best_system) {
                  patrol_fleet->mission.type = FleetMissionType::PatrolSystem;
                  patrol_fleet->mission.patrol_system_id = best_system;
                  patrol_fleet->mission.patrol_dwell_days = 4;
                  patrol_fleet->mission.patrol_leg_index = 0;
                }
              }
            }
          }
        }

        ensure_fleet_mission_defaults(*patrol_fleet);
        (void)configure_fleet_formation(patrol_fleet->id, FleetFormation::LineAbreast, 3.0);
      }

      // Any remaining combatants (should be rare) funnel into patrol, else defense.
      while (take_idx < unassigned_combatants.size()) {
        const Id sid = take_next();
        if (sid == kInvalidId) break;
        const Id target_flid = patrol_fleet ? patrol_fleet->id : (defense_fleet ? defense_fleet->id : kInvalidId);
        if (target_flid == kInvalidId) break;
        std::string err;
        (void)add_ship_to_fleet(target_flid, sid, &err);
      }
    }
  }



  // --- Fleet missions (automation) ---
  {
    NEBULA4X_TRACE_SCOPE("tick_fleet_missions", "sim.ai");

    const int now_day = static_cast<int>(state_.date.days_since_epoch());
    const auto fleet_ids = sorted_keys(state_.fleets);

    auto is_overrideable_order = [&](const Order& o) -> bool {
      return std::holds_alternative<OrbitBody>(o) || std::holds_alternative<WaitDays>(o) ||
             std::holds_alternative<MoveToPoint>(o) || std::holds_alternative<MoveToBody>(o) ||
             std::holds_alternative<EscortShip>(o);
    };

    auto fleet_orders_overrideable = [&](const Fleet& fl) -> bool {
      for (Id sid : fl.ship_ids) {
        auto it = state_.ship_orders.find(sid);
        if (it == state_.ship_orders.end()) continue;
        if (it->second.queue.empty()) continue;
        if (!is_overrideable_order(it->second.queue.front())) return false;
      }
      return true;
    };

    auto fleet_all_orders_empty = [&](const Fleet& fl) -> bool {
      for (Id sid : fl.ship_ids) {
        if (!orders_empty(sid)) return false;
      }
      return true;
    };

    // For non-combat missions (e.g. explore), we don't want to constantly
    // override movement orders. Consider the fleet "retaskable" only when
    // it's idle or parked (orbiting / waiting).
    auto is_parked_order = [&](const Order& ord) -> bool {
      return std::holds_alternative<OrbitBody>(ord) || std::holds_alternative<WaitDays>(ord);
    };

    auto fleet_is_idle_or_parked = [&](const Fleet& fl) -> bool {
      for (Id sid : fl.ship_ids) {
        auto it = state_.ship_orders.find(sid);
        if (it == state_.ship_orders.end()) continue;
        const auto& q = it->second.queue;
        if (q.empty()) continue;
        if (!is_parked_order(q.front())) return false;
      }
      return true;
    };


    auto pick_fleet_leader = [&](Fleet& fl) -> Ship* {
      Ship* leader = find_ptr(state_.ships, fl.leader_ship_id);
      if (leader && leader->faction_id == fl.faction_id) return leader;
      for (Id sid : fl.ship_ids) {
        Ship* sh = find_ptr(state_.ships, sid);
        if (!sh) continue;
        if (sh->faction_id != fl.faction_id) continue;
        fl.leader_ship_id = sid;
        return sh;
      }
      return nullptr;
    };

    auto fleet_min_speed_km_s = [&](const Fleet& fl, double fallback) -> double {
      double slowest = std::numeric_limits<double>::infinity();
      for (Id sid : fl.ship_ids) {
        const Ship* sh = find_ptr(state_.ships, sid);
        if (!sh) continue;
        if (sh->speed_km_s <= 0.0) continue;
        slowest = std::min(slowest, sh->speed_km_s);
      }
      if (!std::isfinite(slowest)) return fallback;
      return slowest;
    };

    auto ship_fuel_fraction = [&](const Ship& sh) -> double {
      const ShipDesign* d = find_design(sh.design_id);
      if (!d) return 1.0;
      const double cap = std::max(0.0, d->fuel_capacity_tons);
      if (cap <= 1e-9) return 1.0;
      const double fuel = (sh.fuel_tons < 0.0) ? cap : std::clamp(sh.fuel_tons, 0.0, cap);
      return std::clamp(fuel / cap, 0.0, 1.0);
    };

    auto ship_hp_fraction = [&](const Ship& sh) -> double {
      const ShipDesign* d = find_design(sh.design_id);
      const double max_hp = d ? std::max(0.0, d->max_hp) : std::max(0.0, sh.hp);
      if (max_hp <= 1e-9) return 1.0;
      const double hp = std::clamp(sh.hp, 0.0, max_hp);
      const double frac = std::clamp(hp / max_hp, 0.0, 1.0);

      // Fold subsystem integrity into "effective HP" so AI repair heuristics don't
      // ignore critical engine/weapon/sensor damage (from combat or maintenance).
      auto clamp01 = [](double x) -> double {
        if (!std::isfinite(x)) return 1.0;
        return std::clamp(x, 0.0, 1.0);
      };
      const double avg_subsys =
          0.25 * (clamp01(sh.engines_integrity) + clamp01(sh.weapons_integrity) + clamp01(sh.sensors_integrity) +
                  clamp01(sh.shields_integrity));
      return std::clamp(frac * avg_subsys, 0.0, 1.0);
    };

    auto ship_missile_ammo_fraction = [&](const Ship& sh) -> double {
      const ShipDesign* d = find_design(sh.design_id);
      const int cap = d ? std::max(0, d->missile_ammo_capacity) : 0;
      if (cap <= 0) return 1.0;
      int ammo = sh.missile_ammo;
      if (ammo < 0) ammo = cap;
      ammo = std::clamp(ammo, 0, cap);
      return std::clamp(static_cast<double>(ammo) / static_cast<double>(cap), 0.0, 1.0);
    };

    auto ship_maintenance_fraction = [&](const Ship& sh) -> double {
      if (!cfg_.enable_ship_maintenance) return 1.0;
      return std::clamp(sh.maintenance_condition, 0.0, 1.0);
    };

    auto colony_resource_production_per_day = [&](const Colony& c, const std::string& resource_id) -> double {
      double total = 0.0;
      for (const auto& [inst_id, count] : c.installations) {
        if (count <= 0) continue;
        auto itdef = content_.installations.find(inst_id);
        if (itdef == content_.installations.end()) continue;
        const auto& def = itdef->second;
        auto itp = def.produces_per_day.find(resource_id);
        if (itp == def.produces_per_day.end()) continue;
        total += static_cast<double>(count) * std::max(0.0, itp->second);
      }
      return total;
    };

    auto select_refuel_colony_for_fleet = [&](Id fleet_faction_id, Id start_sys, Vec2 start_pos, double speed_km_s) -> Id {
      if (speed_km_s <= 0.0) return kInvalidId;

      Id best_cid = kInvalidId;
      double best_eta = std::numeric_limits<double>::infinity();
      bool best_has_fuel = false;

      for (Id cid : sorted_keys(state_.colonies)) {
        const Colony* c = find_ptr(state_.colonies, cid);
        if (!c) continue;
        if (!are_factions_trade_partners(fleet_faction_id, c->faction_id)) continue;
        const Body* b = find_ptr(state_.bodies, c->body_id);
        if (!b) continue;
        if (b->system_id == kInvalidId) continue;
        if (!is_system_discovered_by_faction(fleet_faction_id, b->system_id)) continue;

        const double eta = estimate_eta_days_to_pos(start_sys, start_pos, fleet_faction_id, speed_km_s,
                                                    b->system_id, b->position_mkm);
        if (!std::isfinite(eta)) continue;

        const double fuel_avail = [&]() {
          auto it = c->minerals.find("Fuel");
          return (it != c->minerals.end()) ? std::max(0.0, it->second) : 0.0;
        }();
        const bool has_fuel = (fuel_avail > 1e-6);

        if (best_cid == kInvalidId) {
          best_cid = cid;
          best_eta = eta;
          best_has_fuel = has_fuel;
          continue;
        }

        if (has_fuel != best_has_fuel) {
          if (has_fuel && !best_has_fuel) {
            best_cid = cid;
            best_eta = eta;
            best_has_fuel = true;
          }
          continue;
        }

        if (eta + 1e-9 < best_eta || (std::abs(eta - best_eta) <= 1e-9 && cid < best_cid)) {
          best_cid = cid;
          best_eta = eta;
          best_has_fuel = has_fuel;
        }
      }

      return best_cid;
    };

    auto select_repair_colony_for_fleet = [&](const Fleet& fl, Id start_sys, Vec2 start_pos, double speed_km_s) -> Id {
      if (speed_km_s <= 0.0) return kInvalidId;

      // Total damage across the fleet.
      double total_missing_hp = 0.0;
      for (Id sid : fl.ship_ids) {
        const Ship* sh = find_ptr(state_.ships, sid);
        if (!sh) continue;
        if (sh->faction_id != fl.faction_id) continue;
        const ShipDesign* d = find_design(sh->design_id);
        const double max_hp = d ? std::max(0.0, d->max_hp) : std::max(0.0, sh->hp);
        if (max_hp <= 1e-9) continue;
        const double hp = std::clamp(sh->hp, 0.0, max_hp);
        if (hp < max_hp - 1e-9) total_missing_hp += (max_hp - hp);
      }

      if (total_missing_hp <= 1e-9) return kInvalidId;

      Id best_cid = kInvalidId;
      double best_score = std::numeric_limits<double>::infinity();
      int best_yards = 0;

      const double per_yard = std::max(0.0, cfg_.repair_hp_per_day_per_shipyard);
      if (per_yard <= 1e-9) return kInvalidId;

      for (Id cid : sorted_keys(state_.colonies)) {
        const Colony* c = find_ptr(state_.colonies, cid);
        if (!c) continue;
        if (!are_factions_trade_partners(fl.faction_id, c->faction_id)) continue;

        const auto it_yard = c->installations.find("shipyard");
        const int yards = (it_yard != c->installations.end()) ? it_yard->second : 0;
        if (yards <= 0) continue;

        const Body* b = find_ptr(state_.bodies, c->body_id);
        if (!b) continue;
        if (b->system_id == kInvalidId) continue;
        if (!is_system_discovered_by_faction(fl.faction_id, b->system_id)) continue;

        const double eta = estimate_eta_days_to_pos(start_sys, start_pos, fl.faction_id, speed_km_s,
                                                    b->system_id, b->position_mkm);
        if (!std::isfinite(eta)) continue;

        const double repair_time = total_missing_hp / (per_yard * static_cast<double>(yards));
        const double score = eta + repair_time;

        if (best_cid == kInvalidId || score + 1e-9 < best_score ||
            (std::abs(score - best_score) <= 1e-9 && yards > best_yards) ||
            (std::abs(score - best_score) <= 1e-9 && yards == best_yards && cid < best_cid)) {
          best_cid = cid;
          best_score = score;
          best_yards = yards;
        }
      }

      return best_cid;
    };

    auto select_rearm_colony_for_fleet = [&](Id fleet_faction_id, Id start_sys, Vec2 start_pos, double speed_km_s) -> Id {
      if (speed_km_s <= 0.0) return kInvalidId;

      constexpr const char* kMunitionsKey = "Munitions";

      Id best_cid = kInvalidId;
      int best_tier = -1;
      double best_prod = 0.0;
      double best_eta = std::numeric_limits<double>::infinity();

      for (Id cid : sorted_keys(state_.colonies)) {
        const Colony* c = find_ptr(state_.colonies, cid);
        if (!c) continue;
        if (!are_factions_trade_partners(fleet_faction_id, c->faction_id)) continue;
        const Body* b = find_ptr(state_.bodies, c->body_id);
        if (!b) continue;
        if (b->system_id == kInvalidId) continue;
        if (!is_system_discovered_by_faction(fleet_faction_id, b->system_id)) continue;

        const double eta = estimate_eta_days_to_pos(start_sys, start_pos, fleet_faction_id, speed_km_s,
                                                    b->system_id, b->position_mkm);
        if (!std::isfinite(eta)) continue;

        const double mun_avail = [&]() {
          auto it = c->minerals.find(kMunitionsKey);
          return (it != c->minerals.end()) ? std::max(0.0, it->second) : 0.0;
        }();
        const bool has_mun = (mun_avail >= 1.0 - 1e-9);

        const double prod = colony_resource_production_per_day(*c, kMunitionsKey);
        const bool has_prod = (prod > 1e-9);

        const int tier = has_mun ? 2 : (has_prod ? 1 : 0);

        if (best_cid == kInvalidId || tier > best_tier) {
          best_cid = cid;
          best_tier = tier;
          best_prod = prod;
          best_eta = eta;
          continue;
        }

        if (tier != best_tier) continue;

        if (tier == 1) {
          if (prod > best_prod + 1e-9 ||
              (std::abs(prod - best_prod) <= 1e-9 && (eta + 1e-9 < best_eta || (std::abs(eta - best_eta) <= 1e-9 && cid < best_cid)))) {
            best_cid = cid;
            best_prod = prod;
            best_eta = eta;
          }
        } else {
          // Tier 2 (stockpile) or Tier 0 (no stockpile/production): prefer nearest.
          if (eta + 1e-9 < best_eta || (std::abs(eta - best_eta) <= 1e-9 && cid < best_cid)) {
            best_cid = cid;
            best_eta = eta;
            best_prod = prod;
          }
        }
      }

      return best_cid;
    };

    auto select_maintenance_colony_for_fleet = [&](Id fleet_faction_id, Id start_sys, Vec2 start_pos, double speed_km_s) -> Id {
      if (!cfg_.enable_ship_maintenance) return kInvalidId;
      if (cfg_.ship_maintenance_resource_id.empty()) return kInvalidId;
      if (speed_km_s <= 0.0) return kInvalidId;

      const std::string& kMaintKey = cfg_.ship_maintenance_resource_id;

      Id best_cid = kInvalidId;
      int best_tier = -1;
      double best_avail = 0.0;
      double best_prod = 0.0;
      double best_eta = std::numeric_limits<double>::infinity();

      for (Id cid : sorted_keys(state_.colonies)) {
        const Colony* c = find_ptr(state_.colonies, cid);
        if (!c) continue;
        if (!are_factions_trade_partners(fleet_faction_id, c->faction_id)) continue;
        const Body* b = find_ptr(state_.bodies, c->body_id);
        if (!b) continue;
        if (b->system_id == kInvalidId) continue;
        if (!is_system_discovered_by_faction(fleet_faction_id, b->system_id)) continue;

        const double eta = estimate_eta_days_to_pos(start_sys, start_pos, fleet_faction_id, speed_km_s,
                                                    b->system_id, b->position_mkm);
        if (!std::isfinite(eta)) continue;

        const double avail = [&]() {
          auto it = c->minerals.find(kMaintKey);
          return (it != c->minerals.end()) ? std::max(0.0, it->second) : 0.0;
        }();
        const bool has_stock = (avail > 1e-6);

        const double prod = colony_resource_production_per_day(*c, kMaintKey);
        const bool has_prod = (prod > 1e-9);

        const int tier = has_stock ? 2 : (has_prod ? 1 : 0);

        if (best_cid == kInvalidId || tier > best_tier) {
          best_cid = cid;
          best_tier = tier;
          best_avail = avail;
          best_prod = prod;
          best_eta = eta;
          continue;
        }

        if (tier != best_tier) continue;

        if (tier == 2) {
          // Prefer nearest, then higher stockpile.
          if (eta + 1e-9 < best_eta ||
              (std::abs(eta - best_eta) <= 1e-9 && (avail > best_avail + 1e-9 ||
                                                   (std::abs(avail - best_avail) <= 1e-9 && cid < best_cid)))) {
            best_cid = cid;
            best_eta = eta;
            best_avail = avail;
            best_prod = prod;
          }
        } else if (tier == 1) {
          // Prefer higher production, then nearest.
          if (prod > best_prod + 1e-9 ||
              (std::abs(prod - best_prod) <= 1e-9 && (eta + 1e-9 < best_eta || (std::abs(eta - best_eta) <= 1e-9 && cid < best_cid)))) {
            best_cid = cid;
            best_eta = eta;
            best_avail = avail;
            best_prod = prod;
          }
        } else {
          // Tier 0: just go to the nearest.
          if (eta + 1e-9 < best_eta || (std::abs(eta - best_eta) <= 1e-9 && cid < best_cid)) {
            best_cid = cid;
            best_eta = eta;
            best_avail = avail;
            best_prod = prod;
          }
        }
      }

      return best_cid;
    };

    auto combat_target_priority = [&](ShipRole r) -> int {
      // Bias toward removing armed threats first.
      switch (r) {
        case ShipRole::Combatant: return 0;
        case ShipRole::Freighter: return 1;
        case ShipRole::Surveyor: return 2;
        default: return 3;
      }
    };

    for (Id fid : fleet_ids) {
      Fleet& fl = state_.fleets.at(fid);
      if (fl.mission.type == FleetMissionType::None) continue;

      const Faction* fac = find_ptr(state_.factions, fl.faction_id);
      if (!fac || fac->control == FactionControl::AI_Passive) continue;

      Ship* leader = pick_fleet_leader(fl);
      if (!leader) continue;

      const double fleet_speed = fleet_min_speed_km_s(fl, leader->speed_km_s);

      // --- Sustainment (fleet autonomy) ---
      const double refuel_thr = std::clamp(fl.mission.refuel_threshold_fraction, 0.0, 1.0);
      const double refuel_resume = std::clamp(fl.mission.refuel_resume_fraction, 0.0, 1.0);
      const double repair_thr = std::clamp(fl.mission.repair_threshold_fraction, 0.0, 1.0);
      const double repair_resume = std::clamp(fl.mission.repair_resume_fraction, 0.0, 1.0);
      const double rearm_thr = std::clamp(fl.mission.rearm_threshold_fraction, 0.0, 1.0);
      const double rearm_resume = std::clamp(fl.mission.rearm_resume_fraction, 0.0, 1.0);
      const double maint_thr = std::clamp(fl.mission.maintenance_threshold_fraction, 0.0, 1.0);
      const double maint_resume = std::clamp(fl.mission.maintenance_resume_fraction, 0.0, 1.0);

      bool any_need_refuel = false;
      bool all_refueled = true;
      bool any_need_repair = false;
      bool all_repaired = true;
      bool any_need_rearm = false;
      bool all_rearmed = true;
      bool any_need_maintenance = false;
      bool all_maintained = true;

      for (Id sid : fl.ship_ids) {
        const Ship* sh = find_ptr(state_.ships, sid);
        if (!sh) continue;
        if (sh->faction_id != fl.faction_id) continue;

        const double ffrac = ship_fuel_fraction(*sh);
        if (ffrac + 1e-9 < refuel_thr) any_need_refuel = true;
        if (ffrac + 1e-9 < refuel_resume) all_refueled = false;

        const double hfrac = ship_hp_fraction(*sh);
        if (hfrac + 1e-9 < repair_thr) any_need_repair = true;
        if (hfrac + 1e-9 < repair_resume) all_repaired = false;

        const double afrac = ship_missile_ammo_fraction(*sh);
        if (afrac + 1e-9 < rearm_thr) any_need_rearm = true;
        if (afrac + 1e-9 < rearm_resume) all_rearmed = false;

        const double mfrac = ship_maintenance_fraction(*sh);
        if (mfrac + 1e-9 < maint_thr) any_need_maintenance = true;
        if (mfrac + 1e-9 < maint_resume) all_maintained = false;
      }

      // Apply toggles / global feature flags.
      if (!fl.mission.auto_refuel) {
        any_need_refuel = false;
        all_refueled = true;
      }
      if (!fl.mission.auto_repair) {
        any_need_repair = false;
        all_repaired = true;
      }
      if (!fl.mission.auto_rearm) {
        any_need_rearm = false;
        all_rearmed = true;
      }
      if (!fl.mission.auto_maintenance || !cfg_.enable_ship_maintenance) {
        any_need_maintenance = false;
        all_maintained = true;
      }

      auto clear_sustainment = [&]() {
        fl.mission.sustainment_mode = FleetSustainmentMode::None;
        fl.mission.sustainment_colony_id = kInvalidId;
      };

      // Sustainment state transitions.
      if (fl.mission.sustainment_mode == FleetSustainmentMode::Maintenance && !cfg_.enable_ship_maintenance) {
        clear_sustainment();
      }
      if (fl.mission.sustainment_mode == FleetSustainmentMode::Refuel && all_refueled) {
        clear_sustainment();
      }
      if (fl.mission.sustainment_mode == FleetSustainmentMode::Repair && all_repaired) {
        clear_sustainment();
      }
      if (fl.mission.sustainment_mode == FleetSustainmentMode::Rearm && all_rearmed) {
        clear_sustainment();
      }
      if (fl.mission.sustainment_mode == FleetSustainmentMode::Maintenance && all_maintained) {
        clear_sustainment();
      }

      if (fl.mission.sustainment_mode == FleetSustainmentMode::None) {
        if (any_need_refuel) {
          fl.mission.sustainment_mode = FleetSustainmentMode::Refuel;
          fl.mission.sustainment_colony_id = select_refuel_colony_for_fleet(fl.faction_id, leader->system_id,
                                                                            leader->position_mkm, fleet_speed);
        } else if (any_need_repair) {
          fl.mission.sustainment_mode = FleetSustainmentMode::Repair;
          fl.mission.sustainment_colony_id = select_repair_colony_for_fleet(fl, leader->system_id, leader->position_mkm,
                                                                            fleet_speed);
        } else if (any_need_rearm) {
          fl.mission.sustainment_mode = FleetSustainmentMode::Rearm;
          fl.mission.sustainment_colony_id = select_rearm_colony_for_fleet(fl.faction_id, leader->system_id,
                                                                          leader->position_mkm, fleet_speed);
        } else if (any_need_maintenance) {
          fl.mission.sustainment_mode = FleetSustainmentMode::Maintenance;
          fl.mission.sustainment_colony_id = select_maintenance_colony_for_fleet(fl.faction_id, leader->system_id,
                                                                                leader->position_mkm, fleet_speed);
        }

        if (fl.mission.sustainment_mode != FleetSustainmentMode::None &&
            fl.mission.sustainment_colony_id == kInvalidId) {
          clear_sustainment();
        }
      }

      if (fl.mission.sustainment_mode != FleetSustainmentMode::None) {
        // Maintain or acquire a sustainment dock.
        const Id cid = fl.mission.sustainment_colony_id;
        const Colony* col = find_ptr(state_.colonies, cid);
        const Body* body = col ? find_ptr(state_.bodies, col->body_id) : nullptr;
        const Id sys_id = body ? body->system_id : kInvalidId;

        bool valid = true;
        if (cid == kInvalidId || !col || !body || sys_id == kInvalidId) valid = false;
        if (valid && !are_factions_trade_partners(fl.faction_id, col->faction_id)) valid = false;
        if (valid && !is_system_discovered_by_faction(fl.faction_id, sys_id)) valid = false;

        // Mode-specific validity.
        if (valid && fl.mission.sustainment_mode == FleetSustainmentMode::Repair) {
          const auto it_yard = col->installations.find("shipyard");
          const int yards = (it_yard != col->installations.end()) ? it_yard->second : 0;
          if (yards <= 0) valid = false;
        }
        if (valid && fl.mission.sustainment_mode == FleetSustainmentMode::Maintenance) {
          if (!cfg_.enable_ship_maintenance || cfg_.ship_maintenance_resource_id.empty()) valid = false;
        }

        if (!valid) {
          // Can't sustain here; fall back to no sustainment.
          clear_sustainment();
        } else {
          if (fleet_orders_overrideable(fl)) {
            // Route the fleet to the sustainment colony and keep it docked.
            const double dock_range = std::max(0.0, cfg_.docking_range_mkm);
            const bool leader_docked = (leader->system_id == sys_id) &&
                                       ((leader->position_mkm - body->position_mkm).length() <= dock_range + 1e-9);

            // If we're not docked (or not already orbiting the sustainment body), issue a docking stack.
            bool need_orders = !leader_docked;
            if (!need_orders) {
              auto it_ord = state_.ship_orders.find(leader->id);
              if (it_ord == state_.ship_orders.end() || it_ord->second.queue.empty()) {
                need_orders = true;
              } else if (const auto* ob = std::get_if<OrbitBody>(&it_ord->second.queue.front())) {
                if (ob->body_id != body->id) need_orders = true;
              } else {
                // At the body, but not in orbit; keep docked.
                need_orders = true;
              }
            }

            if (need_orders) {
              (void)clear_fleet_orders(fid);
              (void)issue_fleet_travel_to_system(fid, sys_id, /*restrict_to_discovered=*/true);
              (void)issue_fleet_move_to_body(fid, body->id);
              (void)issue_fleet_orbit_body(fid, body->id, /*duration_days=*/-1);
            }
          }

          // Sustainment takes priority over combat/patrol directives.
          continue;
        }
      }

      // --- Mission behavior ---
      if (fl.mission.type == FleetMissionType::DefendColony) {
        const Colony* col = find_ptr(state_.colonies, fl.mission.defend_colony_id);
        const Body* body = col ? find_ptr(state_.bodies, col->body_id) : nullptr;
        if (!col || !body || body->system_id == kInvalidId) continue;

        const Id defend_sys = body->system_id;
        const Vec2 anchor_pos = body->position_mkm;
        const double r_mkm = std::max(0.0, fl.mission.defend_radius_mkm);

        // Look for detected hostiles in the defended system.
        std::vector<Id> hostiles = detected_hostile_ships_in_system(fl.faction_id, defend_sys);
        if (r_mkm > 1e-9) {
          hostiles.erase(std::remove_if(hostiles.begin(), hostiles.end(), [&](Id tid) {
            const Ship* t = find_ptr(state_.ships, tid);
            if (!t) return true;
            return (t->position_mkm - anchor_pos).length() > r_mkm + 1e-9;
          }), hostiles.end());
        }

        if (!hostiles.empty()) {
          // Choose a target (combatants first, then nearest to the defended body).
          Id best = kInvalidId;
          int best_prio = 999;
          double best_dist = 0.0;

          for (Id tid : hostiles) {
            const Ship* tgt = find_ptr(state_.ships, tid);
            if (!tgt) continue;
            const ShipDesign* td = find_design(tgt->design_id);
            const ShipRole tr = td ? td->role : ShipRole::Unknown;
            const int prio = combat_target_priority(tr);
            const double dist = (tgt->position_mkm - anchor_pos).length();

            if (best == kInvalidId || prio < best_prio ||
                (prio == best_prio && (dist < best_dist - 1e-9 ||
                                       (std::abs(dist - best_dist) <= 1e-9 && tid < best)))) {
              best = tid;
              best_prio = prio;
              best_dist = dist;
            }
          }

          if (best != kInvalidId && fleet_orders_overrideable(fl)) {
            (void)clear_fleet_orders(fid);
            (void)issue_fleet_attack_ship(fid, best, /*restrict_to_discovered=*/true);
            fl.mission.last_target_ship_id = best;
          }
          continue;
        }

        // No hostiles: return to / maintain a defensive orbit around the defended body.
        if (fleet_orders_overrideable(fl)) {
          const double dock_range = std::max(0.0, cfg_.docking_range_mkm);
          const bool at_body = (leader->system_id == defend_sys) &&
                               ((leader->position_mkm - anchor_pos).length() <= dock_range + 1e-9);

          bool need_orders = false;
          if (!at_body) {
            need_orders = true;
          } else {
            auto it_ord = state_.ship_orders.find(leader->id);
            if (it_ord == state_.ship_orders.end() || it_ord->second.queue.empty()) {
              need_orders = true;
            } else if (const auto* ob = std::get_if<OrbitBody>(&it_ord->second.queue.front())) {
              if (ob->body_id != body->id) need_orders = true;
            } else {
              need_orders = true;
            }
          }

          if (need_orders) {
            (void)clear_fleet_orders(fid);
            (void)issue_fleet_travel_to_system(fid, defend_sys, /*restrict_to_discovered=*/true);
            (void)issue_fleet_move_to_body(fid, body->id);
            (void)issue_fleet_orbit_body(fid, body->id, /*duration_days=*/-1);
          }
        }

        continue;
      }

      if (fl.mission.type == FleetMissionType::PatrolSystem) {
        Id patrol_sys = fl.mission.patrol_system_id;
        if (patrol_sys == kInvalidId) patrol_sys = leader->system_id;
        if (patrol_sys == kInvalidId) continue;

        const StarSystem* sys = find_ptr(state_.systems, patrol_sys);
        if (!sys) continue;

        // If we're not in the patrol system yet, go there first.
        if (leader->system_id != patrol_sys) {
          if (fleet_orders_overrideable(fl)) {
            (void)clear_fleet_orders(fid);
            (void)issue_fleet_travel_to_system(fid, patrol_sys, /*restrict_to_discovered=*/true);
          }
          continue;
        }

        // Engage detected hostiles in the patrol system.
        const auto hostiles = detected_hostile_ships_in_system(fl.faction_id, patrol_sys);
        if (!hostiles.empty()) {
          Id best = kInvalidId;
          int best_prio = 999;
          double best_dist = 0.0;

          for (Id tid : hostiles) {
            const Ship* tgt = find_ptr(state_.ships, tid);
            if (!tgt) continue;
            const ShipDesign* td = find_design(tgt->design_id);
            const ShipRole tr = td ? td->role : ShipRole::Unknown;
            const int prio = combat_target_priority(tr);
            const double dist = (tgt->position_mkm - leader->position_mkm).length();

            if (best == kInvalidId || prio < best_prio ||
                (prio == best_prio && (dist < best_dist - 1e-9 ||
                                       (std::abs(dist - best_dist) <= 1e-9 && tid < best)))) {
              best = tid;
              best_prio = prio;
              best_dist = dist;
            }
          }

          if (best != kInvalidId && fleet_orders_overrideable(fl)) {
            (void)clear_fleet_orders(fid);
            (void)issue_fleet_attack_ship(fid, best, /*restrict_to_discovered=*/true);
            fl.mission.last_target_ship_id = best;
          }
          continue;
        }

        // Continue patrol when idle.
        if (!fleet_all_orders_empty(fl)) continue;

        // Build a deterministic list of waypoints: prefer jump points, else major bodies, else sit.
        std::vector<Vec2> waypoints;
        waypoints.reserve(sys->jump_points.size());
        for (Id jid : sys->jump_points) {
          const JumpPoint* jp = find_ptr(state_.jump_points, jid);
          if (!jp) continue;
          waypoints.push_back(jp->position_mkm);
        }

        if (waypoints.empty()) {
          for (Id bid : sys->bodies) {
            const Body* b = find_ptr(state_.bodies, bid);
            if (!b) continue;
            if (b->type == BodyType::Asteroid) continue;
            waypoints.push_back(b->position_mkm);
          }
        }

        if (waypoints.empty()) {
          (void)issue_fleet_wait_days(fid, std::max(1, fl.mission.patrol_dwell_days));
          continue;
        }

        const int idx = (fl.mission.patrol_leg_index < 0) ? 0 : fl.mission.patrol_leg_index;
        const int widx = idx % static_cast<int>(waypoints.size());
        fl.mission.patrol_leg_index = widx + 1;

        (void)issue_fleet_move_to_point(fid, waypoints[widx]);
        (void)issue_fleet_wait_days(fid, std::max(1, fl.mission.patrol_dwell_days));
        continue;
      }









      if (fl.mission.type == FleetMissionType::GuardJumpPoint) {
        // GuardJumpPoint: Hold position near a specific jump point, intercepting
        // detected hostiles within a response radius.
        Id jp_id = fl.mission.guard_jump_point_id;
        const JumpPoint* jp = (jp_id != kInvalidId) ? find_ptr(state_.jump_points, jp_id) : nullptr;

        // Best-effort default: pick the lowest-id jump point in the fleet leader's
        // current system when the mission target is unset / invalid.
        if (!jp || jp->system_id == kInvalidId) {
          const StarSystem* lsys = find_ptr(state_.systems, leader->system_id);
          if (lsys) {
            auto jps = lsys->jump_points;
            std::sort(jps.begin(), jps.end());
            for (Id cand : jps) {
              const JumpPoint* jp2 = find_ptr(state_.jump_points, cand);
              if (!jp2) continue;
              if (jp2->system_id == kInvalidId) continue;
              jp_id = cand;
              jp = jp2;
              break;
            }
          }
          fl.mission.guard_jump_point_id = jp_id;
        }

        if (!jp || jp->system_id == kInvalidId) continue;

        const Id guard_sys = jp->system_id;
        const Vec2 anchor_pos = jp->position_mkm;
        const double r_mkm = std::max(0.0, fl.mission.guard_jump_radius_mkm);

        // If we're not in the guard system yet, go there first.
        if (leader->system_id != guard_sys) {
          if (fleet_orders_overrideable(fl)) {
            (void)clear_fleet_orders(fid);
            (void)issue_fleet_travel_to_system(fid, guard_sys, /*restrict_to_discovered=*/true);
          }
          continue;
        }

        // Engage detected hostiles near the guarded jump point.
        std::vector<Id> hostiles = detected_hostile_ships_in_system(fl.faction_id, guard_sys);
        if (r_mkm > 1e-9) {
          hostiles.erase(std::remove_if(hostiles.begin(), hostiles.end(), [&](Id tid) {
            const Ship* t = find_ptr(state_.ships, tid);
            if (!t) return true;
            return (t->position_mkm - anchor_pos).length() > r_mkm + 1e-9;
          }), hostiles.end());
        }

        if (!hostiles.empty()) {
          Id best = kInvalidId;
          int best_prio = 999;
          double best_dist = 0.0;

          for (Id tid : hostiles) {
            const Ship* tgt = find_ptr(state_.ships, tid);
            if (!tgt) continue;
            const ShipDesign* td = find_design(tgt->design_id);
            const ShipRole tr = td ? td->role : ShipRole::Unknown;
            const int prio = combat_target_priority(tr);
            const double dist = (tgt->position_mkm - anchor_pos).length();

            if (best == kInvalidId || prio < best_prio ||
                (prio == best_prio && (dist < best_dist - 1e-9 ||
                                       (std::abs(dist - best_dist) <= 1e-9 && tid < best)))) {
              best = tid;
              best_prio = prio;
              best_dist = dist;
            }
          }

          if (best != kInvalidId && fleet_orders_overrideable(fl)) {
            (void)clear_fleet_orders(fid);
            (void)issue_fleet_attack_ship(fid, best, /*restrict_to_discovered=*/true);

            // Best-effort intel alert (rate-limited to 1/day unless target changes).
            if (best != fl.mission.last_target_ship_id || fl.mission.guard_last_alert_day != now_day) {
              fl.mission.guard_last_alert_day = now_day;

              const auto* sys = find_ptr(state_.systems, guard_sys);
              const auto* tgt = find_ptr(state_.ships, best);
              std::string msg = "Guard: " + fl.name + " intercepting ";
              msg += (tgt ? (tgt->name.empty() ? ("Ship " + std::to_string(best)) : tgt->name)
                          : ("Ship " + std::to_string(best)));
              msg += " near " + jp->name;
              if (sys) msg += " (" + sys->name + ")";

              EventContext ctx;
              ctx.faction_id = fl.faction_id;
              ctx.system_id = guard_sys;
              ctx.ship_id = best;
              push_event(EventLevel::Info, EventCategory::Intel, std::move(msg), ctx);
            }

            fl.mission.last_target_ship_id = best;
          }
          continue;
        }

        // No hostiles: return to / maintain a defensive picket at the jump point.
        if (fleet_orders_overrideable(fl)) {
          const double station_tol = std::max(0.5, cfg_.docking_range_mkm);

          const bool at_anchor = (leader->system_id == guard_sys) &&
                                 ((leader->position_mkm - anchor_pos).length() <= station_tol + 1e-9);

          bool need_orders = false;
          bool already_moving_to_anchor = false;

          auto it_ord = state_.ship_orders.find(leader->id);
          if (it_ord != state_.ship_orders.end() && !it_ord->second.queue.empty()) {
            const auto& ord = it_ord->second.queue.front();
            if (const auto* mv = std::get_if<MoveToPoint>(&ord)) {
              if ((mv->target_mkm - anchor_pos).length() <= station_tol + 1e-9) {
                already_moving_to_anchor = true;
              }
            }
            if (std::holds_alternative<WaitDays>(ord)) {
              already_moving_to_anchor = true;
            }
          }

          if (!at_anchor && already_moving_to_anchor) {
            need_orders = false;
          } else if (!at_anchor) {
            need_orders = true;
          } else {
            if (it_ord == state_.ship_orders.end() || it_ord->second.queue.empty()) {
              need_orders = true;
            } else {
              const auto& ord = it_ord->second.queue.front();
              if (!(std::holds_alternative<WaitDays>(ord) ||
                    (std::holds_alternative<MoveToPoint>(ord)))) {
                need_orders = true;
              }
            }
          }

          if (need_orders) {
            (void)clear_fleet_orders(fid);
            (void)issue_fleet_move_to_point(fid, anchor_pos);
            (void)issue_fleet_wait_days(fid, std::max(1, fl.mission.guard_jump_dwell_days));
          }
        }

        continue;
      }

      if (fl.mission.type == FleetMissionType::PatrolRoute) {
        // PatrolRoute: shuttle between two systems and engage detected hostiles
        // in any system encountered along the path.
        Id a = fl.mission.patrol_route_a_system_id;
        Id b = fl.mission.patrol_route_b_system_id;

        // Best-effort defaults: if unset, seed endpoints from the fleet's current location.
        if (a == kInvalidId) a = leader->system_id;
        if (b == kInvalidId) b = a;
        fl.mission.patrol_route_a_system_id = a;
        fl.mission.patrol_route_b_system_id = b;

        if (a == kInvalidId || b == kInvalidId) continue;

        // Engage detected hostiles in the fleet's *current* system, regardless of
        // whether we are traveling or parked.
        {
          const auto hostiles = detected_hostile_ships_in_system(fl.faction_id, leader->system_id);
          if (!hostiles.empty()) {
            Id best = kInvalidId;
            int best_prio = 999;
            double best_dist = 0.0;

            for (Id tid : hostiles) {
              const Ship* tgt = find_ptr(state_.ships, tid);
              if (!tgt) continue;
              const ShipDesign* td = find_design(tgt->design_id);
              const ShipRole tr = td ? td->role : ShipRole::Unknown;
              const int prio = combat_target_priority(tr);
              const double dist = (tgt->position_mkm - leader->position_mkm).length();

              if (best == kInvalidId || prio < best_prio ||
                  (prio == best_prio && (dist < best_dist - 1e-9 ||
                                         (std::abs(dist - best_dist) <= 1e-9 && tid < best)))) {
                best = tid;
                best_prio = prio;
                best_dist = dist;
              }
            }

            if (best != kInvalidId && fleet_orders_overrideable(fl)) {
              (void)clear_fleet_orders(fid);
              (void)issue_fleet_attack_ship(fid, best, /*restrict_to_discovered=*/true);
              fl.mission.last_target_ship_id = best;
            }
            continue;
          }
        }

        // Determine the current target endpoint.
        const int leg = (fl.mission.patrol_leg_index < 0) ? 0 : fl.mission.patrol_leg_index;
        const bool to_b = ((leg % 2) == 0);
        const Id target_sys = to_b ? b : a;
        const Id next_sys = to_b ? a : b;

        // If we're not in the target system yet, route there.
        if (leader->system_id != target_sys) {
          // TravelViaJump orders are not overrideable, so this won't thrash while in transit.
          if (fleet_orders_overrideable(fl)) {
            (void)clear_fleet_orders(fid);
            (void)issue_fleet_travel_to_system(fid, target_sys, /*restrict_to_discovered=*/true);
          }
          continue;
        }

        // When idle at an endpoint, loiter, then route to the other endpoint.
        if (!fleet_all_orders_empty(fl)) continue;

        const int dwell = std::max(1, fl.mission.patrol_dwell_days);
        (void)issue_fleet_wait_days(fid, dwell);

        bool issued = true;
        if (next_sys != target_sys) {
          issued = issue_fleet_travel_to_system(fid, next_sys, /*restrict_to_discovered=*/true);
        }

        if (issued) {
          fl.mission.patrol_leg_index = leg + 1;
        }
        continue;
      }

      if (fl.mission.type == FleetMissionType::PatrolCircuit) {
        // PatrolCircuit: cycle through a user-defined list of waypoint systems,
        // engaging detected hostiles in the current system.

        // Best-effort defaults: if unset, seed from the fleet's current location.
        auto& wps = fl.mission.patrol_circuit_system_ids;
        wps.erase(std::remove(wps.begin(), wps.end(), kInvalidId), wps.end());
        if (wps.empty() && leader->system_id != kInvalidId) {
          wps.push_back(leader->system_id);
        }
        if (wps.empty()) continue;

        // Engage detected hostiles in the fleet's *current* system, regardless
        // of whether we are traveling or parked.
        {
          const auto hostiles = detected_hostile_ships_in_system(fl.faction_id, leader->system_id);
          if (!hostiles.empty()) {
            Id best = kInvalidId;
            int best_prio = 999;
            double best_dist = 0.0;

            for (Id tid : hostiles) {
              const Ship* tgt = find_ptr(state_.ships, tid);
              if (!tgt) continue;
              const ShipDesign* td = find_design(tgt->design_id);
              const ShipRole tr = td ? td->role : ShipRole::Unknown;
              const int prio = combat_target_priority(tr);
              const double dist = (tgt->position_mkm - leader->position_mkm).length();

              if (best == kInvalidId || prio < best_prio ||
                  (prio == best_prio && (dist < best_dist - 1e-9 ||
                                         (std::abs(dist - best_dist) <= 1e-9 && tid < best)))) {
                best = tid;
                best_prio = prio;
                best_dist = dist;
              }
            }

            if (best != kInvalidId && fleet_orders_overrideable(fl)) {
              (void)clear_fleet_orders(fid);
              (void)issue_fleet_attack_ship(fid, best, /*restrict_to_discovered=*/true);
              fl.mission.last_target_ship_id = best;
            }
            continue;
          }
        }

        const int n = static_cast<int>(wps.size());
        int idx = fl.mission.patrol_leg_index;
        if (idx < 0) idx = 0;
        if (n > 0) idx = idx % n;
        fl.mission.patrol_leg_index = idx;

        // Current target waypoint.
        Id target_sys = wps[idx];
        if (target_sys == kInvalidId) continue;

        // If we're not in the target system yet, route there.
        if (leader->system_id != target_sys) {
          if (fleet_orders_overrideable(fl)) {
            (void)clear_fleet_orders(fid);
            // If the target is unreachable (due to fog-of-war restrictions),
            // fall back to the next reachable waypoint instead of stalling.
            bool issued = issue_fleet_travel_to_system(fid, target_sys, /*restrict_to_discovered=*/true);
            if (!issued && n > 1) {
              for (int step = 1; step < n; ++step) {
                const int nxt = (idx + step) % n;
                const Id cand = wps[nxt];
                if (cand == kInvalidId) continue;
                if (issue_fleet_travel_to_system(fid, cand, /*restrict_to_discovered=*/true)) {
                  fl.mission.patrol_leg_index = nxt;
                  issued = true;
                  break;
                }
              }
            }
            (void)issued;
          }
          continue;
        }

        // When idle at a waypoint, loiter, then route to the next waypoint.
        if (!fleet_all_orders_empty(fl)) continue;

        const int dwell = std::max(1, fl.mission.patrol_dwell_days);
        (void)issue_fleet_wait_days(fid, dwell);

        if (n <= 1) continue;

        bool issued = false;
        int next_idx = idx;
        for (int step = 1; step <= n; ++step) {
          const int nxt = (idx + step) % n;
          const Id cand = wps[nxt];
          if (cand == kInvalidId) continue;
          if (cand == target_sys) continue;
          if (issue_fleet_travel_to_system(fid, cand, /*restrict_to_discovered=*/true)) {
            issued = true;
            next_idx = nxt;
            break;
          }
        }

        if (issued) {
          fl.mission.patrol_leg_index = next_idx;
        }
        continue;
      }



      if (fl.mission.type == FleetMissionType::PatrolRegion) {
        // Region-wide patrol: cycle through discovered systems in a region and
        // visit key waypoints (friendly colonies, then jump points, then major bodies).
        // Responds to detected hostiles anywhere in the region (requires sensor coverage).

        Id rid = fl.mission.patrol_region_id;
        if (rid == kInvalidId) {
          if (const auto* lsys = find_ptr(state_.systems, leader->system_id)) {
            rid = lsys->region_id;
            if (rid != kInvalidId) fl.mission.patrol_region_id = rid;
          }
        }
        if (rid == kInvalidId) continue;
        if (!find_ptr(state_.regions, rid)) continue;

        // Build deterministic list of discovered systems in this region.
        std::vector<Id> region_systems;
        region_systems.reserve(16);
        for (Id sid : sorted_keys(state_.systems)) {
          const auto* rsys = find_ptr(state_.systems, sid);
          if (!rsys) continue;
          if (rsys->region_id != rid) continue;
          if (!is_system_discovered_by_faction(fl.faction_id, sid)) continue;
          region_systems.push_back(sid);
        }
        if (region_systems.empty()) continue;

        // Engage detected hostiles anywhere in the region.
        {
          Id best = kInvalidId;
          int best_prio = 999;
          double best_eta = std::numeric_limits<double>::infinity();

          for (Id sid : region_systems) {
            const auto hostiles = detected_hostile_ships_in_system(fl.faction_id, sid);
            for (Id tid : hostiles) {
              const Ship* tgt = find_ptr(state_.ships, tid);
              if (!tgt) continue;
              const ShipDesign* td = find_design(tgt->design_id);
              const ShipRole tr = td ? td->role : ShipRole::Unknown;
              const int prio = combat_target_priority(tr);
              const double eta = estimate_eta_days_to_pos(leader->system_id, leader->position_mkm,
                                                         fl.faction_id, fleet_speed,
                                                         tgt->system_id, tgt->position_mkm);
              if (!std::isfinite(eta)) continue;

              if (best == kInvalidId || prio < best_prio ||
                  (prio == best_prio && (eta < best_eta - 1e-9 ||
                                         (std::abs(eta - best_eta) <= 1e-9 && tid < best)))) {
                best = tid;
                best_prio = prio;
                best_eta = eta;
              }
            }
          }

          if (best != kInvalidId && fleet_orders_overrideable(fl)) {
            (void)clear_fleet_orders(fid);
            (void)issue_fleet_attack_ship(fid, best, /*restrict_to_discovered=*/true);
            fl.mission.last_target_ship_id = best;
            continue;
          }
        }

        // If we're not in the region yet, route to the nearest discovered system in it.
        const StarSystem* cur_sys = find_ptr(state_.systems, leader->system_id);
        const bool in_region = cur_sys && cur_sys->region_id == rid;
        if (!in_region) {
          if (fleet_orders_overrideable(fl)) {
            Id best_sys = kInvalidId;
            double best_eta = std::numeric_limits<double>::infinity();
            int best_idx = 0;

            for (int i = 0; i < static_cast<int>(region_systems.size()); ++i) {
              const Id sid = region_systems[i];
              const double eta = estimate_eta_days_to_system(leader->system_id, leader->position_mkm,
                                                            fl.faction_id, fleet_speed, sid);
              if (!std::isfinite(eta)) continue;
              if (best_sys == kInvalidId || eta < best_eta - 1e-9 ||
                  (std::abs(eta - best_eta) <= 1e-9 && sid < best_sys)) {
                best_sys = sid;
                best_eta = eta;
                best_idx = i;
              }
            }

            if (best_sys != kInvalidId) {
              (void)clear_fleet_orders(fid);
              (void)issue_fleet_travel_to_system(fid, best_sys, /*restrict_to_discovered=*/true);
              fl.mission.patrol_region_system_index = best_idx;
              fl.mission.patrol_region_waypoint_index = 0;
            }
          }
          continue;
        }

        // Continue patrol only when idle.
        if (!fleet_all_orders_empty(fl)) continue;

        // Determine the current target system in the region.
        const int raw_sys_idx = (fl.mission.patrol_region_system_index < 0) ? 0 : fl.mission.patrol_region_system_index;
        const int sys_idx = raw_sys_idx % static_cast<int>(region_systems.size());
        Id target_sys = region_systems[sys_idx];

        // If we're not in the target system yet, go there.
        if (leader->system_id != target_sys) {
          if (fleet_orders_overrideable(fl)) {
            (void)clear_fleet_orders(fid);
            (void)issue_fleet_travel_to_system(fid, target_sys, /*restrict_to_discovered=*/true);
            fl.mission.patrol_region_waypoint_index = 0;
          }
          continue;
        }

        const StarSystem* psys = find_ptr(state_.systems, target_sys);
        if (!psys) continue;

        // Build deterministic waypoint list: friendly colonies first, then jump points, then major bodies.
        struct PatrolWaypoint {
          bool is_body{false};
          Id body_id{kInvalidId};
          Vec2 point{0.0, 0.0};
        };

        std::vector<PatrolWaypoint> waypoints;
        waypoints.reserve(psys->bodies.size() + psys->jump_points.size());

        std::unordered_set<Id> seen_bodies;
        seen_bodies.reserve(psys->bodies.size() * 2);

        for (Id cid : sorted_keys(state_.colonies)) {
          const Colony* c = find_ptr(state_.colonies, cid);
          if (!c) continue;
          if (!are_factions_trade_partners(fl.faction_id, c->faction_id)) continue;
          const Body* b = find_ptr(state_.bodies, c->body_id);
          if (!b) continue;
          if (b->system_id != target_sys) continue;
          if (seen_bodies.insert(b->id).second) {
            PatrolWaypoint w;
            w.is_body = true;
            w.body_id = b->id;
            waypoints.push_back(w);
          }
        }

        std::vector<Id> jps = psys->jump_points;
        std::sort(jps.begin(), jps.end());
        for (Id jid : jps) {
          const JumpPoint* jp = find_ptr(state_.jump_points, jid);
          if (!jp) continue;
          PatrolWaypoint w;
          w.is_body = false;
          w.point = jp->position_mkm;
          waypoints.push_back(w);
        }

        std::vector<Id> bodies = psys->bodies;
        std::sort(bodies.begin(), bodies.end());
        for (Id bid : bodies) {
          const Body* b = find_ptr(state_.bodies, bid);
          if (!b) continue;
          if (b->type == BodyType::Asteroid) continue;
          if (seen_bodies.insert(b->id).second) {
            PatrolWaypoint w;
            w.is_body = true;
            w.body_id = b->id;
            waypoints.push_back(w);
          }
        }

        if (waypoints.empty()) {
          (void)issue_fleet_wait_days(fid, std::max(1, fl.mission.patrol_region_dwell_days));
          continue;
        }

        // Advance to next system after completing a full waypoint loop.
        int idx = (fl.mission.patrol_region_waypoint_index < 0) ? 0 : fl.mission.patrol_region_waypoint_index;
        int widx = idx % static_cast<int>(waypoints.size());
        const bool wrapped = (idx > 0 && widx == 0);
        if (wrapped) {
          fl.mission.patrol_region_system_index = raw_sys_idx + 1;
          fl.mission.patrol_region_waypoint_index = 0;
          const int nidx = ((fl.mission.patrol_region_system_index < 0) ? 0 : fl.mission.patrol_region_system_index) %
                           static_cast<int>(region_systems.size());
          const Id next_sys = region_systems[nidx];
          if (next_sys != target_sys && fleet_orders_overrideable(fl)) {
            (void)clear_fleet_orders(fid);
            (void)issue_fleet_travel_to_system(fid, next_sys, /*restrict_to_discovered=*/true);
            continue;
          }
          idx = 0;
          widx = 0;
        }

        fl.mission.patrol_region_waypoint_index = widx + 1;

        const int dwell = std::max(1, fl.mission.patrol_region_dwell_days);
        const PatrolWaypoint& w = waypoints[widx];
        if (w.is_body && w.body_id != kInvalidId) {
          (void)issue_fleet_move_to_body(fid, w.body_id);
          (void)issue_fleet_orbit_body(fid, w.body_id, /*duration_days=*/dwell);
        } else {
          (void)issue_fleet_move_to_point(fid, w.point);
          (void)issue_fleet_wait_days(fid, dwell);
        }

        continue;
      }
      if (fl.mission.type == FleetMissionType::Explore) {
        // Only retask when we're idle or parked (avoid fighting movement).
        if (!fleet_is_idle_or_parked(fl)) continue;

        const Id faction_id = fl.faction_id;
        const auto* sys = find_ptr(state_.systems, leader->system_id);
        if (!sys) continue;

        const auto it_cache = explore_cache.find(faction_id);
        const ExploreFactionCache* cache = (it_cache != explore_cache.end()) ? &it_cache->second : nullptr;

        auto& reserved_jumps = reserved_explore_jump_targets[faction_id];
        auto& reserved_frontiers = reserved_explore_frontier_targets[faction_id];
        auto& reserved_anoms = reserved_explore_anomaly_targets[faction_id];
        auto& reserved_wrecks = reserved_explore_wreck_targets[faction_id];

        std::vector<Id> jps = sys->jump_points;
        std::sort(jps.begin(), jps.end());

        const bool survey_first = fl.mission.explore_survey_first;
        const bool allow_transit = fl.mission.explore_allow_transit;

        const bool do_anoms = fl.mission.explore_investigate_anomalies;
        const bool do_wrecks = fl.mission.explore_salvage_wrecks;
        const bool survey_transit_when_done = fl.mission.explore_survey_transit_when_done && allow_transit;

        // Optional: opportunistic anomaly investigation / salvage while exploring.
        // These are only attempted when there are no detected hostiles in the current system
        // (to avoid luring exploration fleets into ambushes).
        const bool system_has_hostiles = !detected_hostile_ships_in_system(faction_id, leader->system_id).empty();

        if (!system_has_hostiles) {
          // (0) Anomalies: if enabled, investigate high-value unresolved anomalies in this system.
          if (do_anoms) {
            const ShipDesign* d = find_design(leader->design_id);
            const double speed_mkm_d = (d && d->speed_km_s > 1e-9)
                                           ? mkm_per_day_from_speed(d->speed_km_s, cfg_.seconds_per_day)
                                           : 1.0;

            Id best_anom = kInvalidId;
            double best_score = -std::numeric_limits<double>::infinity();
            double best_d2 = std::numeric_limits<double>::infinity();

            for (const auto& [aid, a] : state_.anomalies) {
              if (aid == kInvalidId) continue;
              if (a.system_id != leader->system_id) continue;
              if (a.resolved) continue;
              if (!is_anomaly_discovered_by_faction(faction_id, aid)) continue;
              if (reserved_anoms.contains(aid)) continue;

              double minerals_total = 0.0;
              for (const auto& [_, t] : a.mineral_reward) minerals_total += std::max(0.0, t);

              double value = std::max(0.0, a.research_reward);
              value += minerals_total * 0.05;  // heuristic: 20t ~ 1 RP
              if (!a.unlock_component_id.empty()) value += 25.0;

              const double risk = std::clamp(a.hazard_chance, 0.0, 1.0) * std::max(0.0, a.hazard_damage);

              const double d2 = (leader->position_mkm - a.position_mkm).length_squared();
              const double dist = std::sqrt(std::max(0.0, d2));
              const double travel_days = dist / std::max(1e-6, speed_mkm_d);

              const double score = value / (1.0 + travel_days) - risk;

              if (best_anom == kInvalidId || score > best_score + 1e-9 ||
                  (std::abs(score - best_score) <= 1e-9 &&
                   (d2 + 1e-9 < best_d2 || (std::abs(d2 - best_d2) <= 1e-9 && aid < best_anom)))) {
                best_anom = aid;
                best_score = score;
                best_d2 = d2;
              }
            }

            if (best_anom != kInvalidId && fleet_orders_overrideable(fl)) {
              reserved_anoms.insert(best_anom);
              clear_fleet_orders(fid);
              issue_fleet_investigate_anomaly(fid, best_anom, /*restrict_to_discovered=*/true);
              continue;
            }
          }

          // (1) Wreck salvage: if enabled, salvage nearby mineral caches.
          if (do_wrecks) {
            Id best_wreck = kInvalidId;
            double best_d2 = std::numeric_limits<double>::infinity();
            double best_tons = 0.0;

            for (const auto& [wid, w] : state_.wrecks) {
              if (wid == kInvalidId) continue;
              if (w.system_id != leader->system_id) continue;
              if (reserved_wrecks.contains(wid)) continue;

              double total = 0.0;
              for (const auto& [_, t] : w.minerals) total += std::max(0.0, t);
              if (total <= 1e-6) continue;

              const double d2 = (leader->position_mkm - w.position_mkm).length_squared();
              if (best_wreck == kInvalidId || d2 + 1e-9 < best_d2 ||
                  (std::abs(d2 - best_d2) <= 1e-9 &&
                   (total > best_tons + 1e-9 || (std::abs(total - best_tons) <= 1e-9 && wid < best_wreck)))) {
                best_wreck = wid;
                best_d2 = d2;
                best_tons = total;
              }
            }

            if (best_wreck != kInvalidId && fleet_orders_overrideable(fl)) {
              reserved_wrecks.insert(best_wreck);
              clear_fleet_orders(fid);
              issue_fleet_salvage_wreck(fid, best_wreck, /*mineral=*/"", /*tons=*/0.0, /*restrict_to_discovered=*/true);
              continue;
            }
          }
        }

        auto pick_transit_jump = [&]() -> Id {
          if (!allow_transit) return kInvalidId;

          Id best_jump = kInvalidId;
          double best_dist = std::numeric_limits<double>::infinity();
          for (Id jp_id : jps) {
            if (jp_id == kInvalidId) continue;
            if (reserved_jumps.contains(jp_id)) continue;

            if (cache && !cache->surveyed.contains(jp_id)) continue;

            const JumpPoint* jp = find_ptr(state_.jump_points, jp_id);
            if (!jp) continue;
            const JumpPoint* other = find_ptr(state_.jump_points, jp->linked_jump_id);
            if (!other) continue;

            const Id dest_sys = other->system_id;
            if (dest_sys == kInvalidId) continue;

            const bool dest_known = cache ? cache->discovered.contains(dest_sys)
                                          : is_system_discovered_by_faction(faction_id, dest_sys);
            if (dest_known) continue;

            const double dist = (leader->position_mkm - jp->position_mkm).length();
            if (best_jump == kInvalidId || dist + 1e-9 < best_dist ||
                (std::abs(dist - best_dist) <= 1e-9 && jp_id < best_jump)) {
              best_jump = jp_id;
              best_dist = dist;
            }
          }
          return best_jump;
        };

        auto pick_survey_jump = [&]() -> Id {
          Id best_jump = kInvalidId;
          double best_dist = std::numeric_limits<double>::infinity();
          for (Id jp_id : jps) {
            if (jp_id == kInvalidId) continue;
            if (reserved_jumps.contains(jp_id)) continue;

            const bool surveyed = cache ? cache->surveyed.contains(jp_id) : is_jump_point_surveyed_by_faction(faction_id, jp_id);
            if (surveyed) continue;

            const JumpPoint* jp = find_ptr(state_.jump_points, jp_id);
            if (!jp) continue;

            const double dist = (leader->position_mkm - jp->position_mkm).length();
            if (best_jump == kInvalidId || dist + 1e-9 < best_dist ||
                (std::abs(dist - best_dist) <= 1e-9 && jp_id < best_jump)) {
              best_jump = jp_id;
              best_dist = dist;
            }
          }
          return best_jump;
        };

        const Id transit_jump = pick_transit_jump();
        const Id survey_jump = pick_survey_jump();


        auto jump_leads_to_undiscovered = [&](Id jp_id) -> bool {
          const JumpPoint* jp = find_ptr(state_.jump_points, jp_id);
          if (!jp || jp->linked_jump_id == kInvalidId) return false;
          const JumpPoint* other = find_ptr(state_.jump_points, jp->linked_jump_id);
          if (!other) return false;
          const Id dest_sys = other->system_id;
          if (dest_sys == kInvalidId) return false;
          const bool dest_known = cache ? cache->discovered.contains(dest_sys)
                                        : is_system_discovered_by_faction(faction_id, dest_sys);
          return !dest_known;
        };

        auto issue_survey = [&](Id jp_id, bool transit_when_done) {
          if (jp_id == kInvalidId) return;
          reserved_jumps.insert(jp_id);
          clear_fleet_orders(fid);
          issue_fleet_survey_jump_point(fid, jp_id, transit_when_done, /*restrict_to_discovered=*/true);
        };


        auto issue_transit = [&](Id jp_id) {
          if (jp_id == kInvalidId) return;
          reserved_jumps.insert(jp_id);
          clear_fleet_orders(fid);
          issue_fleet_travel_via_jump(fid, jp_id);
        };

        // Local system work first.
        if (survey_first) {
          if (survey_jump != kInvalidId) {
            issue_survey(survey_jump, survey_transit_when_done && jump_leads_to_undiscovered(survey_jump));
            continue;
          }
          if (transit_jump != kInvalidId) {
            issue_transit(transit_jump);
            continue;
          }
        } else {
          if (transit_jump != kInvalidId) {
            issue_transit(transit_jump);
            continue;
          }
          if (survey_jump != kInvalidId) {
            issue_survey(survey_jump, survey_transit_when_done && jump_leads_to_undiscovered(survey_jump));
            continue;
          }
        }

        // No local work: route to the best frontier system.
        if (!cache) continue;

        if (fleet_speed <= 0.0) continue;

        Id best_frontier = kInvalidId;
        double best_score = -1e9;
        for (const auto& fr : cache->frontiers) {
          const Id sys_id = fr.system_id;
          if (sys_id == leader->system_id) continue;
          if (reserved_frontiers.contains(sys_id)) continue;

          const int work = fr.unknown_exits + (allow_transit ? fr.known_exits_to_undiscovered : 0);
          if (work <= 0) continue;

          const double eta = estimate_eta_days_to_system(leader->system_id, leader->position_mkm, faction_id, fleet_speed, sys_id);
          if (!std::isfinite(eta)) continue;

          const double score = static_cast<double>(work) * 1000.0 - eta * 10.0;
          if (best_frontier == kInvalidId || score > best_score + 1e-9 ||
              (std::abs(score - best_score) <= 1e-9 && sys_id < best_frontier)) {
            best_frontier = sys_id;
            best_score = score;
          }
        }

        if (best_frontier != kInvalidId) {
          reserved_frontiers.insert(best_frontier);
          clear_fleet_orders(fid);
          issue_fleet_travel_to_system(fid, best_frontier, /*restrict_to_discovered=*/true);
        }

        continue;
      }

      if (fl.mission.type == FleetMissionType::AssaultColony) {
        const Id target_cid = fl.mission.assault_colony_id;
        const Colony* tgt_col = (target_cid != kInvalidId) ? find_ptr(state_.colonies, target_cid) : nullptr;
        const Body* tgt_body = tgt_col ? find_ptr(state_.bodies, tgt_col->body_id) : nullptr;
        const Id target_sys = tgt_body ? tgt_body->system_id : kInvalidId;
        if (!tgt_col || !tgt_body || target_sys == kInvalidId) continue;

        // Mission complete: colony already belongs to us.
        if (tgt_col->faction_id == fl.faction_id) {
          fl.mission = FleetMission{};
          continue;
        }

        // Can't plan against undiscovered systems.
        if (!is_system_discovered_by_faction(fl.faction_id, target_sys)) continue;

        // Respect treaties that would forbid hostile actions.
        TreatyType tt;
        if (sim_internal::strongest_active_treaty_between(state_, fl.faction_id, tgt_col->faction_id, &tt)) {
          continue;
        }

        // Fleet troop/capability snapshot.
        double embarked_strength = 0.0;
        double troop_capacity_total = 0.0;
        double troop_free_capacity = 0.0;
        bool any_troop_capacity = false;
        bool any_bombard_capable = false;

        auto fleet_ship_ids = fl.ship_ids;
        std::sort(fleet_ship_ids.begin(), fleet_ship_ids.end());

        for (Id sid : fleet_ship_ids) {
          const Ship* sh = find_ptr(state_.ships, sid);
          if (!sh) continue;
          if (sh->faction_id != fl.faction_id) continue;

          embarked_strength += std::max(0.0, sh->troops);

          const ShipDesign* d = find_design(sh->design_id);
          const double cap = d ? std::max(0.0, d->troop_capacity) : 0.0;
          troop_capacity_total += cap;
          troop_free_capacity += std::max(0.0, cap - std::max(0.0, sh->troops));

          if (cap > 1e-9) any_troop_capacity = true;
          if (d && d->weapon_damage > 1e-9 && d->weapon_range_mkm > 1e-9) any_bombard_capable = true;
        }

        embarked_strength = std::max(0.0, embarked_strength);
        troop_capacity_total = std::max(0.0, troop_capacity_total);
        troop_free_capacity = std::max(0.0, troop_free_capacity);

        // Defender snapshot (use active battle state when present).
        double defender_strength = std::max(0.0, tgt_col->ground_forces);
        if (auto itb = state_.ground_battles.find(tgt_col->id); itb != state_.ground_battles.end()) {
          const auto& b = itb->second;
          if (b.attacker_faction_id == fl.faction_id) {
            // We already have an ongoing invasion; don't thrash orders.
            continue;
          }
          if (b.defender_faction_id == tgt_col->faction_id) {
            defender_strength = std::max(0.0, b.defender_strength);
          }
        }

        // Defender fortifications and artillery (installation weapons).
        const double forts = std::max(0.0, fortification_points(*tgt_col));

        double defender_arty_weapon = 0.0;
        for (const auto& [inst_id, count] : tgt_col->installations) {
          if (count <= 0) continue;
          const auto it = content_.installations.find(inst_id);
          if (it == content_.installations.end()) continue;
          const double wd = it->second.weapon_damage;
          if (wd <= 1e-9) continue;
          defender_arty_weapon += wd * static_cast<double>(count);
        }
        defender_arty_weapon = std::max(0.0, defender_arty_weapon);

        const double margin = std::clamp(fl.mission.assault_troop_margin_factor, 1.0, 10.0);
        const double required_strength = std::max(
            0.0,
            square_law_required_attacker_strength(cfg_, defender_strength, forts, defender_arty_weapon, margin));

        // --- 1) Staging / embarkation (best-effort) ---
        const double need_more = std::max(0.0, required_strength - embarked_strength);

        auto is_valid_staging_colony = [&](Id cid) -> bool {
          const Colony* c = find_ptr(state_.colonies, cid);
          if (!c) return false;
          if (c->faction_id != fl.faction_id) return false;
          const Body* b = find_ptr(state_.bodies, c->body_id);
          if (!b || b->system_id == kInvalidId) return false;
          if (!is_system_discovered_by_faction(fl.faction_id, b->system_id)) return false;
          return true;
        };

        auto staging_surplus_strength = [&](Id cid) -> double {
          const Colony* c = find_ptr(state_.colonies, cid);
          if (!c) return 0.0;
          const double desired = std::max(0.0, c->garrison_target_strength);
          return std::max(0.0, std::max(0.0, c->ground_forces) - desired);
        };

        auto pick_best_staging_colony = [&]() -> Id {
          Id best = kInvalidId;
          double best_score = -1e18;
          for (Id cid : sorted_keys(state_.colonies)) {
            const Colony* c = find_ptr(state_.colonies, cid);
            if (!c) continue;
            if (c->faction_id != fl.faction_id) continue;

            const Body* b = find_ptr(state_.bodies, c->body_id);
            if (!b || b->system_id == kInvalidId) continue;
            if (!is_system_discovered_by_faction(fl.faction_id, b->system_id)) continue;

            const double surplus = staging_surplus_strength(cid);
            if (surplus <= 1e-6) continue;

            const double eta = estimate_eta_days_to_pos(leader->system_id, leader->position_mkm,
                                                       fl.faction_id, fleet_speed,
                                                       b->system_id, b->position_mkm);
            if (!std::isfinite(eta)) continue;

            const double score = surplus * 1000.0 - eta * 10.0;
            if (best == kInvalidId || score > best_score + 1e-9 ||
                (std::abs(score - best_score) <= 1e-9 && cid < best)) {
              best = cid;
              best_score = score;
            }
          }
          return best;
        };

        if (need_more > 1e-6 && fl.mission.assault_auto_stage && troop_free_capacity > 1e-6 && any_troop_capacity) {
          Id stage_cid = fl.mission.assault_staging_colony_id;
          if (!is_valid_staging_colony(stage_cid)) {
            stage_cid = pick_best_staging_colony();
            if (stage_cid != kInvalidId) {
              fl.mission.assault_staging_colony_id = stage_cid;
            }
          }

          const double surplus = staging_surplus_strength(stage_cid);
          const double take_frac = std::clamp(cfg_.auto_troop_max_take_fraction_of_surplus, 0.0, 1.0);
          const double take_cap = surplus * take_frac;
          const double to_take = std::min({need_more, troop_free_capacity, take_cap});

          if (stage_cid != kInvalidId && to_take > 1e-6 && fleet_orders_overrideable(fl)) {
            (void)clear_fleet_orders(fid);

            // Bring the whole fleet to the staging colony so escorts don't get left behind.
            if (const Colony* sc = find_ptr(state_.colonies, stage_cid)) {
              (void)issue_fleet_orbit_body(fid, sc->body_id, /*duration_days=*/0,
                                          /*restrict_to_discovered=*/true);
            }

            double remaining = to_take;
            for (Id sid : ship_ids) {
              if (remaining <= 1e-6) break;
              Ship* sh = find_ptr(state_.ships, sid);
              if (!sh) continue;
              if (sh->faction_id != fl.faction_id) continue;
              const ShipDesign* d = find_design(sh->design_id);
              const double cap = d ? std::max(0.0, d->troop_capacity) : 0.0;
              const double free = std::max(0.0, cap - std::max(0.0, sh->troops));
              if (free <= 1e-6) continue;

              const double load = std::min(free, remaining);
              if (load > 1e-6) {
                (void)issue_load_troops(sid, stage_cid, load, /*restrict_to_discovered=*/true);
              }
              remaining -= load;
            }

            // Clear any prior bombard progress when we return to staging.
            fl.mission.assault_bombard_executed = false;
            continue;
          }
        }

        // --- 2) Bombardment (optional, once) ---
        const bool bombard_enabled = fl.mission.assault_use_bombardment && fl.mission.assault_bombard_days != 0;
        if (bombard_enabled && any_bombard_capable && !fl.mission.assault_bombard_executed) {
          if (fleet_orders_overrideable(fl)) {
            const int days = fl.mission.assault_bombard_days;
            (void)clear_fleet_orders(fid);
            (void)issue_fleet_bombard_colony(fid, target_cid, days, /*restrict_to_discovered=*/true);
            fl.mission.assault_bombard_executed = true;
          }
          continue;
        }

        // --- 3) Invasion ---
        if (embarked_strength > 1e-6 && fleet_orders_overrideable(fl)) {
          (void)clear_fleet_orders(fid);
          (void)issue_fleet_orbit_body(fid, tgt_col->body_id, /*duration_days=*/0,
                                      /*restrict_to_discovered=*/true);
          for (Id sid : ship_ids) {
            const Ship* sh = find_ptr(state_.ships, sid);
            if (!sh) continue;
            if (sh->faction_id != fl.faction_id) continue;
            if (sh->troops <= 1e-6) continue;
            (void)issue_invade_colony(sid, target_cid, /*restrict_to_discovered=*/true);
          }
        }

        continue;
      }

      if (fl.mission.type == FleetMissionType::BlockadeColony) {
        const Id target_cid = fl.mission.blockade_colony_id;
        const Colony* tgt_col = (target_cid != kInvalidId) ? find_ptr(state_.colonies, target_cid) : nullptr;
        const Body* tgt_body = tgt_col ? find_ptr(state_.bodies, tgt_col->body_id) : nullptr;
        const Id target_sys = tgt_body ? tgt_body->system_id : kInvalidId;
        if (!tgt_col || !tgt_body || target_sys == kInvalidId) continue;

        // Mission complete/invalid: colony is no longer a hostile target.
        if (tgt_col->faction_id == fl.faction_id || are_factions_trade_partners(fl.faction_id, tgt_col->faction_id) ||
            (!are_factions_hostile(fl.faction_id, tgt_col->faction_id) &&
             !are_factions_hostile(tgt_col->faction_id, fl.faction_id))) {
          fl.mission = FleetMission{};
          continue;
        }

        // Can't plan against undiscovered systems.
        if (!is_system_discovered_by_faction(fl.faction_id, target_sys)) continue;

        const Vec2 anchor_pos = tgt_body->position_mkm;
        double engage_radius = fl.mission.blockade_radius_mkm;
        if (engage_radius <= 0.0) engage_radius = std::max(0.0, cfg_.blockade_radius_mkm);

        // If we're not in the target system yet, go there first.
        if (leader->system_id != target_sys) {
          if (fleet_orders_overrideable(fl)) {
            (void)clear_fleet_orders(fid);
            (void)issue_fleet_travel_to_system(fid, target_sys, /*restrict_to_discovered=*/true);
            (void)issue_fleet_move_to_body(fid, tgt_body->id, /*restrict_to_discovered=*/true);
            (void)issue_fleet_orbit_body(fid, tgt_body->id, /*duration_days=*/-1,
                                        /*restrict_to_discovered=*/true);
          }
          continue;
        }

        // Engage detected hostiles near the target body.
        const auto hostiles = detected_hostile_ships_in_system(fl.faction_id, target_sys);
        if (!hostiles.empty()) {
          Id best = kInvalidId;
          int best_prio = 999;
          double best_dist = 0.0;

          for (Id tid : hostiles) {
            const Ship* tgt = find_ptr(state_.ships, tid);
            if (!tgt) continue;
            if (engage_radius > 0.0) {
              const double dist_anchor = (tgt->position_mkm - anchor_pos).length();
              if (dist_anchor > engage_radius + 1e-9) continue;
            }
            const ShipDesign* td = find_design(tgt->design_id);
            const ShipRole tr = td ? td->role : ShipRole::Unknown;
            const int prio = combat_target_priority(tr);
            const double dist = (tgt->position_mkm - leader->position_mkm).length();

            if (best == kInvalidId || prio < best_prio ||
                (prio == best_prio && (dist < best_dist - 1e-9 ||
                                       (std::abs(dist - best_dist) <= 1e-9 && tid < best)))) {
              best = tid;
              best_prio = prio;
              best_dist = dist;
            }
          }

          if (best != kInvalidId && fleet_orders_overrideable(fl)) {
            (void)clear_fleet_orders(fid);
            (void)issue_fleet_attack_ship(fid, best, /*restrict_to_discovered=*/true);
            fl.mission.last_target_ship_id = best;
          }
          continue;
        }

        // No hostiles: maintain orbit around the target body.
        if (fleet_orders_overrideable(fl)) {
          const double dock_range = std::max(0.0, cfg_.docking_range_mkm);
          const bool at_body = (leader->system_id == target_sys) &&
                               ((leader->position_mkm - anchor_pos).length() <= dock_range + 1e-9);

          bool need_orders = false;
          if (!at_body) {
            need_orders = true;
          } else {
            auto it_ord = state_.ship_orders.find(leader->id);
            if (it_ord == state_.ship_orders.end() || it_ord->second.queue.empty()) {
              need_orders = true;
            } else if (const auto* ob = std::get_if<OrbitBody>(&it_ord->second.queue.front())) {
              if (ob->body_id != tgt_body->id) need_orders = true;
            } else {
              need_orders = true;
            }
          }

          if (need_orders) {
            (void)clear_fleet_orders(fid);
            (void)issue_fleet_move_to_body(fid, tgt_body->id, /*restrict_to_discovered=*/true);
            (void)issue_fleet_orbit_body(fid, tgt_body->id, /*duration_days=*/-1,
                                        /*restrict_to_discovered=*/true);
          }
        }

        continue;
      }

      if (fl.mission.type == FleetMissionType::HuntHostiles) {
        // 1) If hostiles are currently detected in-system, attack.
        const auto hostiles = detected_hostile_ships_in_system(fl.faction_id, leader->system_id);
        if (!hostiles.empty()) {
          Id best = kInvalidId;
          int best_prio = 999;
          double best_dist = 0.0;

          for (Id tid : hostiles) {
            const Ship* tgt = find_ptr(state_.ships, tid);
            if (!tgt) continue;
            const ShipDesign* td = find_design(tgt->design_id);
            const ShipRole tr = td ? td->role : ShipRole::Unknown;
            const int prio = combat_target_priority(tr);
            const double dist = (tgt->position_mkm - leader->position_mkm).length();

            if (best == kInvalidId || prio < best_prio ||
                (prio == best_prio && (dist < best_dist - 1e-9 ||
                                       (std::abs(dist - best_dist) <= 1e-9 && tid < best)))) {
              best = tid;
              best_prio = prio;
              best_dist = dist;
            }
          }

          if (best != kInvalidId && fleet_orders_overrideable(fl)) {
            (void)clear_fleet_orders(fid);
            (void)issue_fleet_attack_ship(fid, best, /*restrict_to_discovered=*/true);
            fl.mission.last_target_ship_id = best;
          }
          continue;
        }

        // 2) Otherwise, pursue the most recent hostile contact within the chase age window.
        const int max_age = std::max(0, fl.mission.hunt_max_contact_age_days);

        const Faction* f = find_ptr(state_.factions, fl.faction_id);
        if (!f) continue;

        Id best_target = kInvalidId;
        int best_age = 0;
        int best_prio = 999;

        for (const auto& [sid, c] : f->ship_contacts) {
          if (sid == kInvalidId) continue;
          if (!find_ptr(state_.ships, sid)) continue;  // don't chase deleted ships
          if (c.system_id == kInvalidId) continue;
          if (!is_system_discovered_by_faction(fl.faction_id, c.system_id)) continue;
          if (!are_factions_hostile(fl.faction_id, c.last_seen_faction_id)) continue;

          const int age = now_day - c.last_seen_day;
          if (age < 0) continue;
          if (age > max_age) continue;

          const Ship* tgt = find_ptr(state_.ships, sid);
          const ShipDesign* td = tgt ? find_design(tgt->design_id) : nullptr;
          const ShipRole tr = td ? td->role : ShipRole::Unknown;
          const int prio = combat_target_priority(tr);

          if (best_target == kInvalidId || age < best_age ||
              (age == best_age && (prio < best_prio || (prio == best_prio && sid < best_target)))) {
            best_target = sid;
            best_age = age;
            best_prio = prio;
          }
        }

        if (best_target != kInvalidId && fleet_orders_overrideable(fl)) {
          (void)clear_fleet_orders(fid);
          (void)issue_fleet_attack_ship(fid, best_target, /*restrict_to_discovered=*/true);
          fl.mission.last_target_ship_id = best_target;
        }

        continue;
      }

      if (fl.mission.type == FleetMissionType::EscortFreighters) {
        // Precompute friendly docking points by system once per planning pass.
        std::unordered_map<Id, std::vector<Vec2>> friendly_docks_by_system;
        friendly_docks_by_system.reserve(state_.colonies.size() * 2 + 8);
        for (Id cid : sorted_keys(state_.colonies)) {
          const Colony* c = find_ptr(state_.colonies, cid);
          if (!c) continue;
          if (!are_factions_trade_partners(fl.faction_id, c->faction_id)) continue;
          const Body* b = find_ptr(state_.bodies, c->body_id);
          if (!b) continue;
          if (b->system_id == kInvalidId) continue;
          friendly_docks_by_system[b->system_id].push_back(b->position_mkm);
        }

        auto ship_is_docked_at_any_friendly_colony = [&](const Ship& sh) -> bool {
          const double dock_range = std::max(0.0, cfg_.docking_range_mkm);
          if (dock_range <= 1e-9) return false;
          auto it = friendly_docks_by_system.find(sh.system_id);
          if (it == friendly_docks_by_system.end()) return false;
          for (const auto& pos : it->second) {
            if ((sh.position_mkm - pos).length() <= dock_range + 1e-9) return true;
          }
          return false;
        };

        auto is_basic_escort_target = [&](Id sid) -> bool {
          const Ship* sh = find_ptr(state_.ships, sid);
          if (!sh) return false;
          if (sid == kInvalidId) return false;
          if (!are_factions_mutual_friendly(fl.faction_id, sh->faction_id)) return false;

          // Only escort civilian-ish roles by default.
          const ShipDesign* d = find_design(sh->design_id);
          const ShipRole r = d ? d->role : ShipRole::Unknown;
          return (r == ShipRole::Freighter || r == ShipRole::Surveyor || r == ShipRole::Unknown);
        };

        auto is_auto_escort_target = [&](Id sid) -> bool {
          const Ship* sh = find_ptr(state_.ships, sid);
          if (!sh) return false;
          if (!is_basic_escort_target(sid)) return false;
          if (fl.mission.escort_only_auto_freight && !sh->auto_freight) return false;
          return true;
        };

        Id escort_target = kInvalidId;

        if (fl.mission.escort_target_ship_id != kInvalidId) {
          if (is_basic_escort_target(fl.mission.escort_target_ship_id)) {
            escort_target = fl.mission.escort_target_ship_id;
            fl.mission.escort_active_ship_id = escort_target;
          } else {
            // Fixed target no longer valid.
            fl.mission.escort_active_ship_id = kInvalidId;
            escort_target = kInvalidId;
          }
        } else {
          // Auto-select an eligible friendly freighter.
          const int interval = std::max(0, fl.mission.escort_retarget_interval_days);
          const bool can_retarget = (interval == 0) || (now_day - fl.mission.escort_last_retarget_day >= interval);

          if (!is_auto_escort_target(fl.mission.escort_active_ship_id)) {
            fl.mission.escort_active_ship_id = kInvalidId;
          }

          escort_target = fl.mission.escort_active_ship_id;

          if (escort_target == kInvalidId || can_retarget) {
            // Pick the best candidate: prefer ships that are currently moving or carrying cargo,
            // then minimize ETA.
            Id best = kInvalidId;
            int best_prio = 999;
            double best_eta = std::numeric_limits<double>::infinity();
            double best_cargo = 0.0;

            for (Id sid : sorted_keys(state_.ships)) {
              const Ship* sh = find_ptr(state_.ships, sid);
              if (!sh) continue;
              if (!is_auto_escort_target(sid)) continue;
              // Avoid escorting ships that are already managed by another fleet.
              if (fleet_for_ship(sid) != kInvalidId) continue;

              const bool moving = !orders_empty(sid);
              const double cargo = cargo_used_tons(*sh);
              const bool has_cargo = cargo > 1e-6;
              const bool docked = (!moving && !has_cargo) ? ship_is_docked_at_any_friendly_colony(*sh) : false;

              int prio = 0;
              if (moving || has_cargo) {
                prio = 0;
              } else if (!docked) {
                prio = 1;
              } else {
                prio = 2;
              }

              const double eta = estimate_eta_days_to_pos(leader->system_id, leader->position_mkm,
                                                         fl.faction_id, fleet_speed,
                                                         sh->system_id, sh->position_mkm);
              if (!std::isfinite(eta)) continue;

              if (best == kInvalidId || prio < best_prio ||
                  (prio == best_prio && (eta < best_eta - 1e-9 ||
                                         (std::abs(eta - best_eta) <= 1e-9 &&
                                          (cargo > best_cargo + 1e-9 ||
                                           (std::abs(cargo - best_cargo) <= 1e-9 && sid < best)))))) {
                best = sid;
                best_prio = prio;
                best_eta = eta;
                best_cargo = cargo;
              }
            }

            if (best != kInvalidId) {
              escort_target = best;
              fl.mission.escort_active_ship_id = best;
              fl.mission.escort_last_retarget_day = now_day;
            }
          }
        }

        if (escort_target == kInvalidId) continue;

        const Ship* escorted = find_ptr(state_.ships, escort_target);
        if (!escorted) {
          fl.mission.escort_active_ship_id = kInvalidId;
          continue;
        }

        const Id escort_sys = escorted->system_id;
        if (escort_sys == kInvalidId) continue;

        // Engage detected hostiles that threaten the escorted ship.
        std::vector<Id> hostiles = detected_hostile_ships_in_system(fl.faction_id, escort_sys);
        const double r_mkm = std::max(0.0, fl.mission.escort_defense_radius_mkm);
        if (r_mkm > 1e-9) {
          hostiles.erase(std::remove_if(hostiles.begin(), hostiles.end(), [&](Id tid) {
            const Ship* t = find_ptr(state_.ships, tid);
            if (!t) return true;
            return (t->position_mkm - escorted->position_mkm).length() > r_mkm + 1e-9;
          }), hostiles.end());
        }

        if (!hostiles.empty()) {
          Id best = kInvalidId;
          int best_prio = 999;
          double best_dist = 0.0;
          for (Id tid : hostiles) {
            const Ship* tgt = find_ptr(state_.ships, tid);
            if (!tgt) continue;
            const ShipDesign* td = find_design(tgt->design_id);
            const ShipRole tr = td ? td->role : ShipRole::Unknown;
            const int prio = combat_target_priority(tr);
            const double dist = (tgt->position_mkm - escorted->position_mkm).length();
            if (best == kInvalidId || prio < best_prio ||
                (prio == best_prio && (dist < best_dist - 1e-9 ||
                                       (std::abs(dist - best_dist) <= 1e-9 && tid < best)))) {
              best = tid;
              best_prio = prio;
              best_dist = dist;
            }
          }

          if (best != kInvalidId && fleet_orders_overrideable(fl)) {
            (void)clear_fleet_orders(fid);
            (void)issue_fleet_attack_ship(fid, best, /*restrict_to_discovered=*/true);
            fl.mission.last_target_ship_id = best;
          }
          continue;
        }

        // No immediate threats: ensure we're escorting the target.
        if (fleet_orders_overrideable(fl)) {
          const double follow = std::max(0.0, fl.mission.escort_follow_distance_mkm);

          bool need_orders = false;
          auto it_ord = state_.ship_orders.find(leader->id);
          if (it_ord == state_.ship_orders.end() || it_ord->second.queue.empty()) {
            need_orders = true;
          } else if (const auto* eo = std::get_if<EscortShip>(&it_ord->second.queue.front())) {
            if (eo->target_ship_id != escorted->id) need_orders = true;
          } else {
            need_orders = true;
          }

          if (need_orders) {
            (void)clear_fleet_orders(fid);
            (void)issue_fleet_escort_ship(fid, escorted->id, follow, /*restrict_to_discovered=*/true);
          }
        }

        continue;
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

        // Pirate hideouts are stationary bases; do not issue roaming/chasing orders.
        if (sh->design_id == "pirate_hideout") continue;

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

  // Ambient civilian shipping (procedural convoys). This runs after AI planning
  // so civilian traffic reacts to newly founded colonies, and before piracy
  // suppression/raids so pirates have fresh targets.
  tick_civilian_trade_convoys();

  // Update region piracy suppression after AI planning, so newly assigned patrol
  // missions take effect immediately for the raid weighting below.
  tick_piracy_suppression();

  // Spawn dynamic pirate raids after AI planning, so raids don't get immediately
  // re-tasked by the same tick's AI logic.
  tick_pirate_raids();

  // --- Diplomacy AI: treaty proposals (offers) ---
  //
  // This is a lightweight negotiation layer: AI factions propose treaties via
  // DiplomaticOffer objects, which must be accepted to become active treaties.
  {
    const int now_day = static_cast<int>(state_.date.days_since_epoch());

    // Rough "power" metric used for ceasefire heuristics.
    std::unordered_map<Id, double> power_by_faction;
    power_by_faction.reserve(faction_ids.size() * 2 + 8);
    for (Id fid : faction_ids) power_by_faction[fid] = 0.0;

    for (Id sid : ship_ids) {
      const Ship* sh = find_ptr(state_.ships, sid);
      if (!sh) continue;
      if (sh->faction_id == kInvalidId) continue;
      const ShipDesign* d = find_design(sh->design_id);
      const double w = d ? std::max(0.0, d->weapon_damage) : 0.0;
      const double m = d ? std::max(0.0, d->mass_tons) : 0.0;
      // Weighted sum; tuned for "relative strength" heuristics only (not combat sim).
      power_by_faction[sh->faction_id] += std::max(1.0, w + m * 0.05);
    }

    auto has_contact = [&](const Faction& f, Id other_faction_id) -> bool {
      if (other_faction_id == kInvalidId) return false;
      for (const auto& [sid, c] : f.ship_contacts) {
        (void)sid;
        if (c.last_seen_faction_id == other_faction_id) return true;
      }
      return false;
    };

    auto has_pending_offer = [&](Id from_id, Id to_id, TreatyType tt) -> bool {
      for (const auto& [oid, o] : state_.diplomatic_offers) {
        (void)oid;
        if (o.from_faction_id == from_id && o.to_faction_id == to_id && o.treaty_type == tt) return true;
      }
      return false;
    };

    auto has_any_pending_offer_between = [&](Id a_id, Id b_id) -> bool {
      for (const auto& [oid, o] : state_.diplomatic_offers) {
        (void)oid;
        if ((o.from_faction_id == a_id && o.to_faction_id == b_id) ||
            (o.from_faction_id == b_id && o.to_faction_id == a_id)) {
          return true;
        }
      }
      return false;
    };

    auto has_active_treaty = [&](Id a_id, Id b_id, TreatyType tt) -> bool {
      if (a_id == kInvalidId || b_id == kInvalidId) return false;
      if (a_id == b_id) return false;
      Id a = a_id;
      Id b = b_id;
      if (b < a) std::swap(a, b);

      for (const auto& [tid, t] : state_.treaties) {
        (void)tid;
        if (t.faction_a == a && t.faction_b == b && t.type == tt) return true;
      }
      return false;
    };

    // 1) Generate offers from AI explorer factions.
    for (Id from_id : faction_ids) {
      Faction* from = find_ptr(state_.factions, from_id);
      if (!from) continue;
      if (from->control != FactionControl::AI_Explorer) continue;

      for (Id to_id : faction_ids) {
        if (to_id == from_id) continue;
        const Faction* to = find_ptr(state_.factions, to_id);
        if (!to) continue;

        // Only propose after some form of contact (prevents "telepathic diplomacy").
        if (!has_contact(*from, to_id)) continue;

        // Cooldown check.
        auto it_cd = from->diplomacy_offer_cooldown_until_day.find(to_id);
        if (it_cd != from->diplomacy_offer_cooldown_until_day.end() && it_cd->second > now_day) continue;

        // Don't clutter with multiple outstanding offers between the same pair.
        if (has_any_pending_offer_between(from_id, to_id)) continue;

        // Decide what to offer.
        TreatyType offer_tt = TreatyType::Ceasefire;
        int offer_treaty_days = -1;
        int offer_expires_days = 30;
        bool should_offer = false;

        const DiplomacyStatus s_from = diplomatic_status(from_id, to_id);
        const DiplomacyStatus s_to = diplomatic_status(to_id, from_id);
        const bool mutual_friendly = (s_from == DiplomacyStatus::Friendly) && (s_to == DiplomacyStatus::Friendly);
        const bool mutual_hostile = (s_from == DiplomacyStatus::Hostile) && (s_to == DiplomacyStatus::Hostile);

        if (mutual_friendly) {
          if (!has_active_treaty(from_id, to_id, TreatyType::TradeAgreement) &&
              !has_pending_offer(from_id, to_id, TreatyType::TradeAgreement)) {
            offer_tt = TreatyType::TradeAgreement;
            offer_treaty_days = -1;
            offer_expires_days = 45;
            should_offer = true;
          } else if (has_active_treaty(from_id, to_id, TreatyType::TradeAgreement) &&
                     !has_active_treaty(from_id, to_id, TreatyType::ResearchAgreement) &&
                     !has_pending_offer(from_id, to_id, TreatyType::ResearchAgreement) &&
                     (now_day % 60 == 0)) {
            // Offer a research agreement as a mid-tier cooperation step.
            offer_tt = TreatyType::ResearchAgreement;
            offer_treaty_days = -1;
            offer_expires_days = 45;
            should_offer = true;
          } else if (has_active_treaty(from_id, to_id, TreatyType::TradeAgreement) &&
                     !has_active_treaty(from_id, to_id, TreatyType::Alliance) &&
                     !has_pending_offer(from_id, to_id, TreatyType::Alliance) &&
                     (now_day % 90 == 0)) {
            // Periodically propose alliance after trade relations exist.
            offer_tt = TreatyType::Alliance;
            offer_treaty_days = -1;
            offer_expires_days = 45;
            should_offer = true;
          }
        } else if (mutual_hostile) {
          // If we are significantly weaker, propose a ceasefire occasionally.
          const double p_from = power_by_faction[from_id];
          const double p_to = power_by_faction[to_id];
          const bool weaker = (p_from + 1.0) < (p_to * 0.75);
          if (weaker && !has_active_treaty(from_id, to_id, TreatyType::Ceasefire) &&
              !has_pending_offer(from_id, to_id, TreatyType::Ceasefire) &&
              (now_day % 30 == 0)) {
            offer_tt = TreatyType::Ceasefire;
            offer_treaty_days = 90;
            offer_expires_days = 20;
            should_offer = true;
          }
        } else {
          // Neutral-ish: suggest a NAP as a low-commitment treaty.
          if (!has_active_treaty(from_id, to_id, TreatyType::NonAggressionPact) &&
              !has_pending_offer(from_id, to_id, TreatyType::NonAggressionPact) &&
              (now_day % 45 == 0)) {
            offer_tt = TreatyType::NonAggressionPact;
            offer_treaty_days = 180;
            offer_expires_days = 30;
            should_offer = true;
          }
        }

        if (!should_offer) continue;

        const bool player_involved = (from->control == FactionControl::Player) || (to->control == FactionControl::Player);
        std::string err;
        const Id oid = create_diplomatic_offer(from_id, to_id, offer_tt, offer_treaty_days, offer_expires_days,
                                               player_involved, &err);
        if (oid != kInvalidId) {
          // Prevent daily spam. The accept/decline path also applies a cooldown.
          constexpr int kCooldownDays = 60;
          from->diplomacy_offer_cooldown_until_day[to_id] = now_day + kCooldownDays;
        }
      }
    }

    // 2) Auto-accept offers addressed to AI recipients.
    if (!state_.diplomatic_offers.empty()) {
      const auto offer_ids = sorted_keys(state_.diplomatic_offers);
      for (Id oid : offer_ids) {
        const DiplomaticOffer* o = find_ptr(state_.diplomatic_offers, oid);
        if (!o) continue;

        const Faction* to = find_ptr(state_.factions, o->to_faction_id);
        const Faction* from = find_ptr(state_.factions, o->from_faction_id);
        if (!to || !from) continue;

        // Player offers require explicit response.
        if (to->control == FactionControl::Player) continue;

        const DiplomacyStatus s_from = diplomatic_status(o->from_faction_id, o->to_faction_id);
        const DiplomacyStatus s_to = diplomatic_status(o->to_faction_id, o->from_faction_id);
        const bool mutual_friendly = (s_from == DiplomacyStatus::Friendly) && (s_to == DiplomacyStatus::Friendly);
        const bool mutual_hostile = (s_from == DiplomacyStatus::Hostile) && (s_to == DiplomacyStatus::Hostile);

        bool accept = false;
        switch (o->treaty_type) {
          case TreatyType::TradeAgreement:
          case TreatyType::ResearchAgreement:
          case TreatyType::NonAggressionPact:
            accept = !mutual_hostile;
            break;
          case TreatyType::Alliance:
            accept = mutual_friendly;
            break;
          case TreatyType::Ceasefire: {
            const double p_to = power_by_faction[o->to_faction_id];
            const double p_from = power_by_faction[o->from_faction_id];
            accept = (p_to + 1.0) < (p_from * 0.85) || (p_from + 1.0) < (p_to * 0.85);
          } break;
        }

        if (accept) {
          std::string err;
          (void)accept_diplomatic_offer(oid, /*push_event=*/false, &err);
        } else {
          // AI declines are silent; the offer will expire naturally.
        }
      }
    }
  }

}


void Simulation::tick_civilian_trade_convoys() {
  if (!cfg_.enable_civilian_trade_convoys) return;
  NEBULA4X_TRACE_SCOPE("tick_civilian_trade_convoys", "sim.civilian_trade_convoys");

  const int max_ships = std::max(0, cfg_.civilian_trade_convoy_max_ships);
  if (max_ships <= 0) return;

  // --- Find or create the neutral merchant faction ---
  constexpr const char* kMerchantFactionName = "Merchant Guild";
  Id merchant_fid = kInvalidId;
  for (const auto& [fid, f] : state_.factions) {
    if (f.control == FactionControl::AI_Passive && f.name == kMerchantFactionName) {
      merchant_fid = fid;
      break;
    }
  }

  if (merchant_fid == kInvalidId) {
    // Snapshot current faction ids before inserting (unordered_map may rehash).
    const auto other_fids = sorted_keys(state_.factions);

    Faction mf;
    mf.id = allocate_id(state_);
    mf.name = kMerchantFactionName;
    mf.control = FactionControl::AI_Passive;

    // Default relations are Hostile (for backward compatibility). Override to
    // neutral with non-pirate factions so the guild can exist without being
    // immediately attacked. Keep pirates hostile.
    for (Id ofid : other_fids) {
      const auto* of = find_ptr(state_.factions, ofid);
      if (!of) continue;
      const DiplomacyStatus st = (of->control == FactionControl::AI_Pirate) ? DiplomacyStatus::Hostile
                                                                            : DiplomacyStatus::Neutral;
      mf.relations[ofid] = st;
    }

    state_.factions[mf.id] = mf;
    merchant_fid = mf.id;

    // Mirror relations on the existing factions.
    for (Id ofid : other_fids) {
      auto* of = find_ptr(state_.factions, ofid);
      if (!of) continue;
      const DiplomacyStatus st = (of->control == FactionControl::AI_Pirate) ? DiplomacyStatus::Hostile
                                                                            : DiplomacyStatus::Neutral;
      of->relations[merchant_fid] = st;
    }
  }

  // --- Determine how many convoys to maintain ---
  int current = 0;
  for (const auto& [sid, sh] : state_.ships) {
    (void)sid;
    if (sh.hp <= 0.0) continue;
    if (sh.faction_id == merchant_fid) current++;
  }

  // Compute trade lanes and scale the convoy target to trade activity.
  TradeNetworkOptions opt;
  opt.include_uncolonized_markets = false;
  opt.max_lanes = std::max(1, cfg_.civilian_trade_convoy_consider_top_lanes);
  opt.max_goods_per_lane = 3;

  const TradeNetwork net = compute_trade_network(*this, opt);
  if (net.lanes.empty()) return;

  // Precompute a "hub" body position per system for more natural spawns.
  std::unordered_map<Id, Vec2> hub_pos;
  hub_pos.reserve(state_.systems.size() * 2 + 8);
  std::unordered_map<Id, double> hub_pop;
  hub_pop.reserve(state_.systems.size() * 2 + 8);
  std::unordered_map<Id, Id> hub_colony;
  hub_colony.reserve(state_.systems.size() * 2 + 8);

  for (const auto& [cid, c] : state_.colonies) {
    const Body* b = find_ptr(state_.bodies, c.body_id);
    if (!b) continue;
    const Id sys = b->system_id;
    if (sys == kInvalidId) continue;
    const double p = std::max(0.0, c.population_millions);
    auto it = hub_pop.find(sys);
    if (it == hub_pop.end() || p > it->second + 1e-9) {
      hub_pop[sys] = p;
      hub_pos[sys] = b->position_mkm;
      hub_colony[sys] = cid;
    }
  }

  auto good_to_resource = [](TradeGoodKind g) -> std::string {
    switch (g) {
      case TradeGoodKind::RawMetals:        return "Duranium";
      case TradeGoodKind::ProcessedMetals:  return "Metals";
      case TradeGoodKind::RawMinerals:      return "Mercassium";
      case TradeGoodKind::ProcessedMinerals:return "Minerals";
      case TradeGoodKind::Volatiles:        return "Sorium";
      case TradeGoodKind::Fuel:             return "Fuel";
      case TradeGoodKind::Munitions:        return "Munitions";
      case TradeGoodKind::Exotics:          return "Corbomite";
      default:                              return std::string();
    }
  };

  // Candidate civilian freighter designs (content ids). Use whichever exist.
  std::vector<std::string> design_pool;
  for (const char* id : {"freighter_beta", "freighter_alpha_ion", "freighter_alpha"}) {
    if (find_design(id)) design_pool.push_back(id);
  }
  if (design_pool.empty()) return;

  // Deterministic daily seed.
  std::uint64_t rng = static_cast<std::uint64_t>(state_.date.days_since_epoch());
  rng ^= static_cast<std::uint64_t>(merchant_fid) * 0x9e3779b97f4a7c15ULL;

  // Approximate blockade pressure per system (max over colonies in that system).
  std::unordered_map<Id, double> blockade_pressure_by_system;
  const double blockade_risk_w = std::max(0.0, cfg_.civilian_trade_convoy_blockade_risk_weight);
  const double loss_risk_w = std::max(0.0, cfg_.civilian_trade_convoy_shipping_loss_risk_weight);
  if (loss_risk_w > 1e-12) {
    // Warm the cache once (cheap) to avoid repeated per-lane recompute.
    ensure_civilian_shipping_loss_cache_current();
  }
  if (cfg_.enable_blockades && blockade_risk_w > 1e-12) {
    blockade_pressure_by_system.reserve(state_.systems.size() * 2 + 8);
    ensure_blockade_cache_current();
    for (const auto& [cid, c] : state_.colonies) {
      (void)cid;
      const Body* b = find_ptr(state_.bodies, c.body_id);
      if (!b) continue;
      const Id sys = b->system_id;
      if (sys == kInvalidId) continue;
      const auto it_bs = blockade_cache_.find(c.id);
      const double p = (it_bs != blockade_cache_.end()) ? std::clamp(it_bs->second.pressure, 0.0, 1.0) : 0.0;
      auto it = blockade_pressure_by_system.find(sys);
      if (it == blockade_pressure_by_system.end() || p > it->second + 1e-12) {
        blockade_pressure_by_system[sys] = p;
      }
    }
  }

  // Copy lanes into a local working list for weighted sampling without
  // replacement (helps spread convoys across multiple corridors).
  struct LanePick { Id from; Id to; double w; std::vector<TradeGoodFlow> flows; };
  std::vector<LanePick> lanes;
  lanes.reserve(net.lanes.size());
  double weighted_total_vol = 0.0;
  for (const auto& l : net.lanes) {
    if (!(l.total_volume > 1e-9)) continue;

    double w = std::max(0.0, l.total_volume);

    // Convoys bias toward safer corridors. Risk is endpoint-weighted (cheap,
    // deterministic) and automatically responds to piracy suppression.
    const double ra = piracy_risk_for_system(l.from_system_id);
    const double rb = piracy_risk_for_system(l.to_system_id);
    double risk = 0.5 * (ra + rb);

    // Blockade disruption also deters civilian traffic.
    if (blockade_risk_w > 1e-12 && !blockade_pressure_by_system.empty()) {
      const double ba = blockade_pressure_by_system.contains(l.from_system_id)
                           ? blockade_pressure_by_system[l.from_system_id]
                           : 0.0;
      const double bb = blockade_pressure_by_system.contains(l.to_system_id)
                           ? blockade_pressure_by_system[l.to_system_id]
                           : 0.0;
      const double blockade_risk = 0.5 * (ba + bb);
      risk += blockade_risk_w * blockade_risk;
    }

    // Recent merchant losses deter traffic even if pirates are no longer
    // present (insurance / confidence effect).
    if (loss_risk_w > 1e-12) {
      const double la = civilian_shipping_loss_pressure_for_system(l.from_system_id);
      const double lb = civilian_shipping_loss_pressure_for_system(l.to_system_id);
      const double loss_risk = 0.5 * (la + lb);
      risk += loss_risk_w * loss_risk;
    }
    risk = std::clamp(risk, 0.0, 1.0);

    const double av = std::clamp(cfg_.civilian_trade_convoy_risk_aversion, 0.0, 1.0);
    const double min_mult = std::clamp(cfg_.civilian_trade_convoy_min_risk_weight, 0.0, 1.0);
    const double mult = std::clamp(1.0 - av * risk, min_mult, 1.0);
    w *= mult;

    if (!(w > 1e-12)) continue;
    lanes.push_back({l.from_system_id, l.to_system_id, w, l.top_flows});
    weighted_total_vol += w;
  }
  if (lanes.empty()) return;

  if (!(weighted_total_vol > 1e-9)) return;

  // Determine how many convoys to maintain. We use risk-weighted trade volume
  // so unsafe corridors naturally reduce civilian traffic.
  int target =
      static_cast<int>(std::llround(std::sqrt(weighted_total_vol) * cfg_.civilian_trade_convoy_target_sqrt_mult));
  target = std::clamp(target, std::max(0, cfg_.civilian_trade_convoy_min_ships), max_ships);

  if (current >= target) return;

  const int max_spawn = std::max(0, cfg_.civilian_trade_convoy_max_spawn_per_day);
  const int spawn_n = std::min({target - current, max_spawn, max_ships - current});
  if (spawn_n <= 0) return;

  auto remove_ship = [&](Id ship_id) {
    auto it = state_.ships.find(ship_id);
    if (it == state_.ships.end()) return;
    const Id sys_id = it->second.system_id;
    if (sys_id != kInvalidId) {
      auto it_sys = state_.systems.find(sys_id);
      if (it_sys != state_.systems.end()) {
        auto& vec = it_sys->second.ships;
        vec.erase(std::remove(vec.begin(), vec.end(), ship_id), vec.end());
      }
    }
    state_.ships.erase(it);
    state_.ship_orders.erase(ship_id);
  };

  auto pick_weighted_lane_index = [&](std::uint64_t& s) -> int {
    double sum = 0.0;
    for (const auto& l : lanes) sum += l.w;
    if (!(sum > 1e-12)) return -1;
    const double r = u01(s) * sum;
    double acc = 0.0;
    for (int i = 0; i < static_cast<int>(lanes.size()); ++i) {
      acc += lanes[i].w;
      if (r <= acc + 1e-12) return i;
    }
    return static_cast<int>(lanes.size()) - 1;
  };

  for (int i = 0; i < spawn_n; ++i) {
    if (lanes.empty()) break;
    const int idx = pick_weighted_lane_index(rng);
    if (idx < 0 || idx >= static_cast<int>(lanes.size())) break;

    const LanePick lp = lanes[idx];
    lanes.erase(lanes.begin() + idx);

    if (lp.from == kInvalidId || lp.to == kInvalidId) continue;
    if (lp.from == lp.to) continue;
    if (!state_.systems.contains(lp.from) || !state_.systems.contains(lp.to)) continue;

    const int d = static_cast<int>(std::floor(u01(rng) * static_cast<double>(design_pool.size())));
    const std::string design_id = design_pool[std::clamp(d, 0, static_cast<int>(design_pool.size()) - 1)];
    const ShipDesign* sd = find_design(design_id);
    if (!sd) continue;

    // Spawn near the primary colony in the origin system (if any), otherwise at
    // the system origin.
    Vec2 anchor{0.0, 0.0};
    if (auto it = hub_pos.find(lp.from); it != hub_pos.end()) anchor = it->second;
    const double ang = u01(rng) * kTwoPi;
    const double rad = 0.15 + 0.25 * u01(rng); // mkm offset from anchor
    const Vec2 spawn_pos{anchor.x + std::cos(ang) * rad, anchor.y + std::sin(ang) * rad};

    Ship sh;
    sh.id = allocate_id(state_);
    sh.faction_id = merchant_fid;
    sh.system_id = lp.from;
    sh.name = "Merchant Convoy " + std::to_string(static_cast<unsigned long long>(sh.id));
    sh.position_mkm = spawn_pos;
    sh.design_id = design_id;
    sh.sensor_mode = SensorMode::Passive;

    const double fill = std::clamp(cfg_.civilian_trade_convoy_cargo_fill_fraction, 0.0, 1.0);
    const double cap = std::max(0.0, sd->cargo_tons);
    const double load = cap * fill;

    if (!cfg_.enable_civilian_trade_convoy_cargo_transfers) {
      // Cosmetic cargo (also provides salvage if the convoy is destroyed).
      if (load > 1e-6 && !lp.flows.empty()) {
        const int n = std::min(3, static_cast<int>(lp.flows.size()));
        double sum_share = 0.0;
        for (int j = 0; j < n; ++j) sum_share += std::max(0.0, lp.flows[j].volume);
        if (!(sum_share > 1e-9)) sum_share = 1.0;

        for (int j = 0; j < n; ++j) {
          const auto res = good_to_resource(lp.flows[j].good);
          if (res.empty()) continue;
          const double part = load * (std::max(0.0, lp.flows[j].volume) / sum_share);
          if (part > 1e-9) sh.cargo[res] += part;
        }
      }
    } else {
      // Real cargo is loaded/unloaded via orders at colony hubs; start empty.
      // Any cargo carried by the convoy is therefore "real" and salvageable.
    }

    // Insert into state.
    state_.ships[sh.id] = sh;
    state_.ship_orders[sh.id] = ShipOrders{};
    state_.systems.at(lp.from).ships.push_back(sh.id);
    apply_design_stats_to_ship(state_.ships.at(sh.id));

    // Build a simple loop: from -> to -> wait -> back -> wait, repeat forever.
    const int wait_base = std::max(0, cfg_.civilian_trade_convoy_endpoint_wait_days_base);
    const int wait_jit = std::max(0, cfg_.civilian_trade_convoy_endpoint_wait_days_jitter);
    const int wait_a = wait_base + static_cast<int>(std::floor(u01(rng) * (wait_jit + 1)));
    const int wait_b = wait_base + static_cast<int>(std::floor(u01(rng) * (wait_jit + 1)));

    auto& q = state_.ship_orders[sh.id].queue;

    if (cfg_.enable_civilian_trade_convoy_cargo_transfers) {
      const Id from_col = [&]() -> Id {
        auto it = hub_colony.find(lp.from);
        return (it != hub_colony.end()) ? it->second : kInvalidId;
      }();
      const Id to_col = [&]() -> Id {
        auto it = hub_colony.find(lp.to);
        return (it != hub_colony.end()) ? it->second : kInvalidId;
      }();

      auto push_load_plan = [&](Id colony_id, const std::vector<TradeGoodFlow>& flows) {
        if (colony_id == kInvalidId) return;
        if (!(load > 1e-6)) return;
        if (flows.empty()) return;

        const int n = std::min(3, static_cast<int>(flows.size()));
        double sum_share = 0.0;
        for (int j = 0; j < n; ++j) sum_share += std::max(0.0, flows[j].volume);
        if (!(sum_share > 1e-9)) sum_share = 1.0;

        std::unordered_map<std::string, double> want;
        want.reserve(static_cast<size_t>(n) * 2 + 4);
        for (int j = 0; j < n; ++j) {
          const std::string res = good_to_resource(flows[j].good);
          if (res.empty()) continue;
          const double part = load * (std::max(0.0, flows[j].volume) / sum_share);
          if (part > 1e-9) want[res] += part;
        }
        if (want.empty()) return;

        std::vector<std::string> keys;
        keys.reserve(want.size());
        for (const auto& [k, _] : want) keys.push_back(k);
        std::sort(keys.begin(), keys.end());

        for (const auto& k : keys) {
          const double tons = want[k];
          if (tons > 1e-9) q.push_back(LoadMineral{.colony_id = colony_id, .mineral = k, .tons = tons});
        }
      };

      // Forward leg: load at origin hub, travel, unload at destination hub.
      push_load_plan(from_col, lp.flows);

      if (!issue_travel_to_system(sh.id, lp.to, /*restrict_to_discovered=*/false)) {
        remove_ship(sh.id);
        continue;
      }
      if (to_col != kInvalidId) q.push_back(UnloadMineral{.colony_id = to_col, .mineral = "", .tons = 0.0});
      if (wait_a > 0) q.push_back(WaitDays{.days_remaining = wait_a});

      // Return leg: try to use the reverse lane's flows if present.
      std::vector<TradeGoodFlow> flows_back = lp.flows;
      for (const auto& l : net.lanes) {
        if (l.from_system_id == lp.to && l.to_system_id == lp.from) {
          flows_back = l.top_flows;
          break;
        }
      }
      push_load_plan(to_col, flows_back);

      if (!issue_travel_to_system(sh.id, lp.from, /*restrict_to_discovered=*/false)) {
        remove_ship(sh.id);
        continue;
      }
      if (from_col != kInvalidId) q.push_back(UnloadMineral{.colony_id = from_col, .mineral = "", .tons = 0.0});
      if (wait_b > 0) q.push_back(WaitDays{.days_remaining = wait_b});

      enable_order_repeat(sh.id, -1);
    } else {
      if (!issue_travel_to_system(sh.id, lp.to, /*restrict_to_discovered=*/false)) {
        remove_ship(sh.id);
        continue;
      }
      if (wait_a > 0) q.push_back(WaitDays{.days_remaining = wait_a});

      if (!issue_travel_to_system(sh.id, lp.from, /*restrict_to_discovered=*/false)) {
        remove_ship(sh.id);
        continue;
      }
      if (wait_b > 0) q.push_back(WaitDays{.days_remaining = wait_b});

      enable_order_repeat(sh.id, -1);
    }
  }
}


void Simulation::tick_piracy_suppression() {
  if (!cfg_.enable_pirate_suppression) return;
  if (state_.regions.empty()) return;

  const double scale = std::max(1e-6, cfg_.pirate_suppression_power_scale);
  const double adj = std::clamp(cfg_.pirate_suppression_adjust_fraction_per_day, 0.0, 1.0);

  // Accumulate patrol power by region id from fleets currently on explicit patrol
  // missions and physically present within the region.
  std::unordered_map<Id, double> patrol_power;
  patrol_power.reserve(state_.regions.size() * 2 + 8);

  auto ship_patrol_power = [&](Id ship_id) -> double {
    const auto* sh = find_ptr(state_.ships, ship_id);
    if (!sh) return 0.0;

    const auto* d = find_design(sh->design_id);
    if (!d) return 0.0;

    // Ignore unarmed hulls (freighters, tankers, etc.). We treat suppression as
    // "combat presence" rather than mere traffic.
    const double weapons =
        std::max(0.0, d->weapon_damage) + std::max(0.0, d->missile_damage) +
        0.5 * std::max(0.0, d->point_defense_damage);
    if (weapons <= 0.0) return 0.0;

    // Small bonuses so "tough" escorts contribute slightly more than paper
    // patrol boats, and long-range sensors help maintain regional security.
    const double durability = 0.05 * std::max(0.0, d->max_hp + d->max_shields);
    const double sensors = 0.02 * std::max(0.0, d->sensor_range_mkm);

    return weapons + durability + sensors;
  };

  const auto fleet_ids = sorted_keys(state_.fleets);
  for (Id fid : fleet_ids) {
    const auto* fl = find_ptr(state_.fleets, fid);
    if (!fl) continue;
    if (fl->ship_ids.empty()) continue;

    const auto* fac = find_ptr(state_.factions, fl->faction_id);
    if (!fac) continue;
    if (fac->control == FactionControl::AI_Pirate) continue;

    // Count patrol missions that represent an active security presence.
    // PatrolRoute contributes suppression to whichever region the fleet is
    // currently traversing.
    const Id leader_id =
        (fl->leader_ship_id != kInvalidId)
            ? fl->leader_ship_id
            : (fl->ship_ids.empty() ? kInvalidId : fl->ship_ids.front());
    const auto* leader = find_ptr(state_.ships, leader_id);
    if (!leader) continue;

    const auto* sys_here = find_ptr(state_.systems, leader->system_id);
    if (!sys_here) continue;

    Id rid = kInvalidId;
    if (fl->mission.type == FleetMissionType::PatrolRegion) {
      rid = fl->mission.patrol_region_id;
      // Require the fleet to actually be in the region right now; otherwise we'd
      // suppress regions from across the galaxy while the fleet is still in transit.
      if (rid == kInvalidId) continue;
      if (sys_here->region_id != rid) continue;
    } else if (fl->mission.type == FleetMissionType::PatrolSystem) {
      const auto* psys = find_ptr(state_.systems, fl->mission.patrol_system_id);
      if (psys) rid = psys->region_id;
      if (rid == kInvalidId) continue;
      if (sys_here->region_id != rid) continue;
    } else if (fl->mission.type == FleetMissionType::PatrolRoute) {
      rid = sys_here->region_id;
    } else if (fl->mission.type == FleetMissionType::PatrolCircuit) {
      rid = sys_here->region_id;
    } else if (fl->mission.type == FleetMissionType::GuardJumpPoint) {
      const auto* jp = find_ptr(state_.jump_points, fl->mission.guard_jump_point_id);
      if (!jp) continue;
      if (jp->system_id != leader->system_id) continue;
      rid = sys_here->region_id;
    } else {
      continue;
    }
    if (rid == kInvalidId) continue;

    double fp = 0.0;
    for (Id sid : fl->ship_ids) fp += ship_patrol_power(sid);
    if (fp > 0.0) patrol_power[rid] += fp;
  }

  for (auto& [rid, reg] : state_.regions) {
    const auto it = patrol_power.find(rid);
    const double power = (it != patrol_power.end()) ? it->second : 0.0;
    const double target = 1.0 - std::exp(-power / scale);

    const double cur = std::clamp(reg.pirate_suppression, 0.0, 1.0);
    const double next = std::clamp(cur + (target - cur) * adj, 0.0, 1.0);
    reg.pirate_suppression = next;
  }
}

void Simulation::tick_pirate_raids() {
  if (!cfg_.enable_pirate_raids) return;

  const int now_day = static_cast<int>(state_.date.days_since_epoch());

  const auto faction_ids = sorted_keys(state_.factions);
  std::vector<Id> pirate_factions;
  std::vector<Id> player_factions;
  pirate_factions.reserve(4);
  player_factions.reserve(2);
  for (Id fid : faction_ids) {
    const auto* fac = find_ptr(state_.factions, fid);
    if (!fac) continue;
    if (fac->control == FactionControl::AI_Pirate) pirate_factions.push_back(fid);
    if (fac->control == FactionControl::Player) player_factions.push_back(fid);
  }
  if (pirate_factions.empty()) return;

  auto target_ship_value = [&](ShipRole r) -> double {
    switch (r) {
      case ShipRole::Freighter: return 6.0;
      case ShipRole::Surveyor: return 3.0;
      case ShipRole::Combatant: return 1.0;
      default: return 1.0;
    }
  };

  auto target_ship_priority = [&](ShipRole r) -> int {
    // Pirates prefer easy prey first.
    switch (r) {
      case ShipRole::Freighter: return 0;
      case ShipRole::Surveyor: return 1;
      case ShipRole::Combatant: return 2;
      default: return 3;
    }
  };

  // --- Trade exposure (piracy target bias) ---
  // Pirates gravitate toward rich markets and high-throughput trade corridors.
  // We precompute lightweight per-system trade signals once per tick and use
  // them to amplify target scores (without creating targets out of nothing).
  std::unordered_map<Id, double> trade_market_size;
  std::unordered_map<Id, double> trade_hub_score;
  std::unordered_map<Id, double> trade_traffic;

  // Hub positions for route planning: pick the most populous colony body per system.
  std::unordered_map<Id, Vec2> system_hub_pos;
  std::unordered_map<Id, double> system_hub_pop;

  system_hub_pos.reserve(state_.systems.size() * 2 + 8);
  system_hub_pop.reserve(state_.systems.size() * 2 + 8);

  for (const auto& [cid, col] : state_.colonies) {
    (void)cid;
    const auto* body = find_ptr(state_.bodies, col.body_id);
    if (!body) continue;
    if (body->system_id == kInvalidId) continue;
    const double pop = std::max(0.0, col.population_millions);
    auto it = system_hub_pop.find(body->system_id);
    if (it == system_hub_pop.end() || pop > it->second + 1e-9) {
      system_hub_pop[body->system_id] = pop;
      system_hub_pos[body->system_id] = body->position_mkm;
    }
  }

  {
    TradeNetworkOptions opt;
    opt.include_uncolonized_markets = false;
    opt.max_lanes = 64;
    TradeNetwork tn = compute_trade_network(*this, opt);

    trade_market_size.reserve(tn.nodes.size() * 2 + 8);
    trade_hub_score.reserve(tn.nodes.size() * 2 + 8);
    for (const auto& n : tn.nodes) {
      trade_market_size[n.system_id] = std::max(0.0, n.market_size);
      trade_hub_score[n.system_id] = std::clamp(n.hub_score, 0.0, 1.0);
    }

    // Approximate corridor traffic by distributing top lane volumes across their
    // planned jump routes. This biases raids toward choke points (not just endpoints).
    if (!tn.lanes.empty()) {
      std::vector<const TradeLane*> lanes;
      lanes.reserve(tn.lanes.size());
      for (const auto& l : tn.lanes) lanes.push_back(&l);

      const std::size_t cap = std::min<std::size_t>(24, lanes.size());
      std::partial_sort(lanes.begin(), lanes.begin() + cap, lanes.end(),
                        [&](const TradeLane* a, const TradeLane* b) {
                          if (std::abs(a->total_volume - b->total_volume) > 1e-9) {
                            return a->total_volume > b->total_volume;
                          }
                          if (a->from_system_id != b->from_system_id) return a->from_system_id < b->from_system_id;
                          return a->to_system_id < b->to_system_id;
                        });

      trade_traffic.reserve(cap * 4 + 8);

      for (std::size_t i = 0; i < cap; ++i) {
        const TradeLane& l = *lanes[i];
        if (l.from_system_id == kInvalidId || l.to_system_id == kInvalidId) continue;
        if (l.from_system_id == l.to_system_id) continue;
        const double vol = std::max(0.0, l.total_volume);
        if (!(vol > 1e-9)) continue;

        Vec2 start_pos{0.0, 0.0};
        if (auto it = system_hub_pos.find(l.from_system_id); it != system_hub_pos.end()) {
          start_pos = it->second;
        }
        std::optional<Vec2> goal_pos;
        if (auto it = system_hub_pos.find(l.to_system_id); it != system_hub_pos.end()) {
          goal_pos = it->second;
        }

        const auto plan = plan_jump_route_cached(l.from_system_id, start_pos, /*faction_id=*/kInvalidId,
                                                 /*speed_km_s=*/1000.0, l.to_system_id,
                                                 /*restrict_to_discovered=*/false, goal_pos);
        if (!plan) continue;
        if (plan->systems.empty()) continue;

        for (Id sys_id : plan->systems) {
          if (sys_id == kInvalidId) continue;
          trade_traffic[sys_id] += vol;
        }
      }
    }
  }


  struct SysAcc {
    double score{0.0};

    // Mobile pirate ships currently present in the system (raiders, etc).
    int pirate_ships{0};

    // Persistent pirate bases ("hideouts") currently present in the system.
    int pirate_hideouts{0};

    // Stable reference to a hideout ship for anchoring spawns (lowest id).
    Id hideout_ship_id{kInvalidId};
    Vec2 hideout_pos{0.0, 0.0};
  };

  for (Id pirate_fid : pirate_factions) {
    auto* pirate_fac = find_ptr(state_.factions, pirate_fid);
    if (!pirate_fac) continue;


    // Prune expired hideout cooldowns (keeps saves small / avoids unbounded growth).
    if (!pirate_fac->pirate_hideout_cooldown_until_day.empty()) {
      for (auto it = pirate_fac->pirate_hideout_cooldown_until_day.begin();
           it != pirate_fac->pirate_hideout_cooldown_until_day.end();) {
        if (it->first == kInvalidId || it->second <= now_day) {
          it = pirate_fac->pirate_hideout_cooldown_until_day.erase(it);
        } else {
          ++it;
        }
      }
    }

    // Hard cap per pirate faction to keep raids from exploding in long games.
    const int max_total = std::max(0, cfg_.pirate_raid_max_total_ships_per_faction);
    int pirate_ship_count = 0;
    int pirate_hideout_count = 0;

    std::unordered_map<Id, SysAcc> acc;
    acc.reserve(state_.systems.size() * 2 + 8);

    // Aggregate ship-based target value and current pirate presence per system.
    for (const auto& [sid, sh] : state_.ships) {
      if (sh.hp <= 0.0) continue;
      if (sh.system_id == kInvalidId) continue;

      SysAcc& a = acc[sh.system_id];

      if (sh.faction_id == pirate_fid) {
        if (sh.design_id == "pirate_hideout") {
          ++a.pirate_hideouts;
          ++pirate_hideout_count;
          if (a.hideout_ship_id == kInvalidId || sid < a.hideout_ship_id) {
            a.hideout_ship_id = sid;
            a.hideout_pos = sh.position_mkm;
          }
        } else {
          ++a.pirate_ships;
          ++pirate_ship_count;
        }
        continue;
      }

      if (!are_factions_hostile(pirate_fid, sh.faction_id)) continue;
      const auto* d = find_design(sh.design_id);
      const ShipRole r = d ? d->role : ShipRole::Unknown;
      a.score += target_ship_value(r);
    }

    // Aggregate colony value per system (pirates love raiding settled worlds).
    for (const auto& [cid, col] : state_.colonies) {
      if (!are_factions_hostile(pirate_fid, col.faction_id)) continue;
      const auto* body = find_ptr(state_.bodies, col.body_id);
      if (!body) continue;
      if (body->system_id == kInvalidId) continue;

      SysAcc& a = acc[body->system_id];

      // Lightly scale with population so "big" colonies draw more attention.
      const double pop = std::max(0.0, col.population_millions);
      a.score += 8.0 + std::sqrt(pop) * 0.25;
    }


    // Amplify target scores based on trade wealth / corridor throughput.
    // This nudges pirates toward rich hubs and busy lanes without creating targets
    // in otherwise empty systems.
    if (!trade_market_size.empty() || !trade_hub_score.empty() || !trade_traffic.empty()) {
      for (auto& [sys_id, a] : acc) {
        if (a.score <= 1e-9) continue;

        double market = 0.0;
        if (auto it = trade_market_size.find(sys_id); it != trade_market_size.end()) market = it->second;

        double hub = 0.0;
        if (auto it = trade_hub_score.find(sys_id); it != trade_hub_score.end()) hub = it->second;
        hub = std::clamp(hub, 0.0, 1.0);

        double traffic = 0.0;
        if (auto it = trade_traffic.find(sys_id); it != trade_traffic.end()) traffic = it->second;

        // Normalize with saturating curves to avoid runaway weights.
        const double market_norm = market / (market + 10.0);
        const double traffic_norm = traffic / (traffic + 20.0);

        double mult = 1.0 + 0.75 * market_norm + 0.60 * traffic_norm + 0.35 * hub;
        mult = std::clamp(mult, 1.0, 3.0);
        a.score *= mult;
      }
    }

    if (max_total > 0 && pirate_ship_count >= max_total) continue;

    // Build candidate target systems.
    std::vector<Id> sys_ids;
    sys_ids.reserve(acc.size());
    for (const auto& [sys_id, _] : acc) sys_ids.push_back(sys_id);
    std::sort(sys_ids.begin(), sys_ids.end());

    struct Candidate {
      Id system_id{kInvalidId};
      double weight{0.0};
      double risk{0.0};
      double score{0.0};

      int pirate_hideouts{0};
      Id hideout_ship_id{kInvalidId};
      Vec2 hideout_pos{0.0, 0.0};
    };

    std::vector<Candidate> candidates;
    candidates.reserve(sys_ids.size());

    double total_weight = 0.0;
    double max_risk = 0.0;

    const int max_pirates_in_sys = std::max(0, cfg_.pirate_raid_max_existing_pirate_ships_in_target_system);
    const double risk_exp = std::max(0.1, cfg_.pirate_raid_risk_exponent);

    for (Id sys_id : sys_ids) {
      const auto it = acc.find(sys_id);
      if (it == acc.end()) continue;
      const SysAcc& a = it->second;

      if (a.score <= 1e-9) continue;
      if (a.pirate_ships > max_pirates_in_sys) continue;

      const double risk = ambient_piracy_risk_for_system(sys_id);
      if (risk <= 1e-6) continue;

      double weight = std::pow(risk, risk_exp) * a.score;
      if (cfg_.enable_pirate_hideouts && a.pirate_hideouts > 0) {
        const double mult = std::max(1.0, cfg_.pirate_hideout_system_weight_multiplier);
        weight *= mult;
      }
      if (weight <= 1e-12) continue;

      Candidate c;
      c.system_id = sys_id;
      c.weight = weight;
      c.risk = risk;
      c.score = a.score;
      c.pirate_hideouts = a.pirate_hideouts;
      c.hideout_ship_id = a.hideout_ship_id;
      c.hideout_pos = a.hideout_pos;
      candidates.push_back(c);

      total_weight += weight;
      max_risk = std::max(max_risk, risk);
    }

    if (candidates.empty() || total_weight <= 1e-12) continue;

    // Deterministic per-day roll.
    std::uint64_t rng = 0xD1B54A32D192ED03ULL;
    rng ^= static_cast<std::uint64_t>(now_day) * 0x9e3779b97f4a7c15ULL;
    rng ^= static_cast<std::uint64_t>(pirate_fid) * 0xbf58476d1ce4e5b9ULL;

    const double base = std::clamp(cfg_.pirate_raid_base_chance_per_day, 0.0, 1.0);
    if (base <= 1e-9) continue;

    // Scale chance by:
    //  - "headroom" under the per-faction ship cap,
    //  - the best piracy risk available today,
    //  - and a saturation curve for total_weight (target availability).
    double cap_headroom = 1.0;
    if (max_total > 0) {
      cap_headroom = std::clamp(1.0 - (static_cast<double>(pirate_ship_count) / static_cast<double>(max_total)), 0.0,
                                1.0);
    }

    const double saturation = total_weight / (total_weight + 60.0);
    double p = base * cap_headroom * (0.30 + 0.70 * max_risk) * (0.50 + 0.50 * saturation);
    p = std::clamp(p, 0.0, 1.0);

    if (u01(rng) >= p) continue;

    // Pick a target system by weight.
    const double pick = u01(rng) * total_weight;
    double running = 0.0;
    Candidate chosen;
    for (const auto& c : candidates) {
      running += c.weight;
      if (running + 1e-12 >= pick) {
        chosen = c;
        break;
      }
    }
    if (chosen.system_id == kInvalidId) chosen = candidates.back();

    StarSystem* sys = find_ptr(state_.systems, chosen.system_id);
    if (!sys) continue;

    // Choose a concrete target inside the system: prefer ships (esp. freighters), otherwise colonies.
    std::vector<Id> best_ships;
    int best_prio = 999;

    for (const auto& [sid, sh] : state_.ships) {
      if (sh.hp <= 0.0) continue;
      if (sh.system_id != chosen.system_id) continue;
      if (!are_factions_hostile(pirate_fid, sh.faction_id)) continue;
      if (sh.faction_id == pirate_fid) continue;

      const auto* d = find_design(sh.design_id);
      const ShipRole r = d ? d->role : ShipRole::Unknown;
      const int prio = target_ship_priority(r);

      if (prio < best_prio) {
        best_prio = prio;
        best_ships.clear();
        best_ships.push_back(sid);
      } else if (prio == best_prio) {
        best_ships.push_back(sid);
      }
    }

    Id target_ship_id = kInvalidId;
    Vec2 target_pos{0.0, 0.0};
    if (!best_ships.empty()) {
      std::sort(best_ships.begin(), best_ships.end());
      const std::size_t idx = rand_index(rng, best_ships.size());
      target_ship_id = best_ships[idx];
      if (const auto* tgt = find_ptr(state_.ships, target_ship_id)) {
        target_pos = tgt->position_mkm;
      }
    }

    Id target_colony_id = kInvalidId;
    if (target_ship_id == kInvalidId) {
      double best_pop = -1.0;
      for (const auto& [cid, col] : state_.colonies) {
        if (!are_factions_hostile(pirate_fid, col.faction_id)) continue;
        const auto* body = find_ptr(state_.bodies, col.body_id);
        if (!body) continue;
        if (body->system_id != chosen.system_id) continue;

        const double pop = std::max(0.0, col.population_millions);
        if (pop > best_pop + 1e-9 || (std::abs(pop - best_pop) <= 1e-9 && cid < target_colony_id)) {
          best_pop = pop;
          target_colony_id = cid;
          target_pos = body->position_mkm;
        }
      }
    }

    if (target_ship_id == kInvalidId && target_colony_id == kInvalidId) continue;

    // Spawn near an existing pirate hideout if present (ambush around the base).
    // Otherwise, spawn near the closest jump point; if none exist, spawn near target.
    Vec2 anchor = target_pos;
    if (cfg_.enable_pirate_hideouts && chosen.hideout_ship_id != kInvalidId) {
      anchor = chosen.hideout_pos;
    } else {
      double best_jp_dist = 1e100;
      for (Id jp_id : sys->jump_points) {
        const auto* jp = find_ptr(state_.jump_points, jp_id);
        if (!jp) continue;
        const double d = (jp->position_mkm - target_pos).length();
        if (d < best_jp_dist) {
          best_jp_dist = d;
          anchor = jp->position_mkm;
        }
      }
    }

    // Determine raid size within remaining cap.
    const int remaining = (max_total > 0) ? (max_total - pirate_ship_count) : cfg_.pirate_raid_max_spawn_ships;
    if (remaining <= 0) continue;

    int min_spawn = std::max(1, cfg_.pirate_raid_min_spawn_ships);
    int max_spawn = std::max(min_spawn, cfg_.pirate_raid_max_spawn_ships);
    max_spawn = std::min(max_spawn, remaining);
    min_spawn = std::min(min_spawn, max_spawn);

    int desired = 1;
    if (chosen.risk >= 0.65) ++desired;
    if (chosen.score >= 14.0) ++desired;
    desired = std::clamp(desired, min_spawn, max_spawn);
    if (desired < max_spawn && u01(rng) < 0.25) ++desired;
    desired = std::min(desired, max_spawn);

    // Raider design pool (scales up slowly over time).
    std::vector<std::string> design_pool;
    const int tier = (now_day >= 365 * 8) ? 2 : (now_day >= 365 * 3 ? 1 : 0);
    if (tier >= 2) {
      design_pool = {"pirate_raider_mk2", "pirate_raider_ion", "pirate_raider"};
    } else if (tier == 1) {
      design_pool = {"pirate_raider_ion", "pirate_raider"};
    } else {
      design_pool = {"pirate_raider"};
    }

    auto choose_design_id = [&](std::uint64_t& r) -> std::string {
      // Try a random start index, then fall back through the pool.
      const std::size_t n = design_pool.size();
      if (n == 0) return std::string();
      const std::size_t start = rand_index(r, n);
      for (std::size_t i = 0; i < n; ++i) {
        const std::string& id = design_pool[(start + i) % n];
        if (find_design(id)) return id;
      }
      return std::string();
    };

    // Optional log event, gated behind player discovery to avoid spoilers.
    if (cfg_.pirate_raid_log_event && !player_factions.empty()) {
      bool visible = false;
      for (Id pf : player_factions) {
        if (is_system_discovered_by_faction(pf, chosen.system_id)) {
          visible = true;
          break;
        }
      }

      if (visible) {
        EventContext ctx;
        ctx.faction_id = pirate_fid;
        ctx.system_id = chosen.system_id;
        if (target_ship_id != kInvalidId) ctx.ship_id = target_ship_id;
        if (target_colony_id != kInvalidId) ctx.colony_id = target_colony_id;

        std::string msg = "Pirate raid activity detected in ";
        msg += sys->name.empty() ? std::string("(unknown system)") : sys->name;
        push_event(EventLevel::Info, EventCategory::General, std::move(msg), ctx);
      }
    }

    // Spawn the ships.
    int spawned_raiders = 0;
    for (int i = 0; i < desired; ++i) {
      const std::string design_id = choose_design_id(rng);
      if (design_id.empty()) break;

      Ship ship;
      ship.id = allocate_id(state_);
      ship.faction_id = pirate_fid;
      ship.system_id = chosen.system_id;
      ship.design_id = design_id;
      ship.name = "Pirate Raider " + std::to_string(ship.id);
      ship.sensor_mode = SensorMode::Active;

      // Spawn a small random offset from the anchor.
      const double ang = u01(rng) * kTwoPi;
      const double rad = 0.5 + u01(rng) * 2.0;
      ship.position_mkm = anchor + Vec2{std::cos(ang), std::sin(ang)} * rad;

      state_.ships.emplace(ship.id, ship);
      state_.ship_orders.emplace(ship.id, ShipOrders{});

      // Add to system ship list for sensors/combat.
      sys->ships.push_back(ship.id);

      // Initialize derived stats for freshly spawned ships.
      if (auto* sh = find_ptr(state_.ships, ship.id)) {
        apply_design_stats_to_ship(*sh);
      }

      ++spawned_raiders;

      // Queue raid orders.
      auto& orders = state_.ship_orders[ship.id];
      if (target_ship_id != kInvalidId) {
        AttackShip ord;
        ord.target_ship_id = target_ship_id;
        ord.has_last_known = true;
        ord.last_known_position_mkm = target_pos;
        orders.queue.push_back(ord);
      } else if (target_colony_id != kInvalidId) {
        BombardColony ord;
        ord.colony_id = target_colony_id;
        // Short, punchy raids rather than endless bombardments.
        ord.duration_days = 4 + static_cast<int>(u01(rng) * 6.0);
        orders.queue.push_back(ord);
      }
    }

    // Optionally establish a pirate hideout in the raided system.
    if (cfg_.enable_pirate_hideouts && spawned_raiders > 0 && chosen.hideout_ship_id == kInvalidId) {
      const int max_hideouts = std::max(0, cfg_.pirate_hideout_max_total_per_faction);
      if (max_hideouts <= 0 || pirate_hideout_count < max_hideouts) {
        int until_day = 0;
        if (auto it_cd = pirate_fac->pirate_hideout_cooldown_until_day.find(chosen.system_id);
            it_cd != pirate_fac->pirate_hideout_cooldown_until_day.end()) {
          until_day = it_cd->second;
        }
        if (until_day <= now_day) {
          const double chance = std::clamp(cfg_.pirate_hideout_establish_chance_per_raid, 0.0, 1.0);
          if (chance > 1e-9 && u01(rng) < chance) {
            if (find_design("pirate_hideout")) {
              Ship hideout;
              hideout.id = allocate_id(state_);
              hideout.faction_id = pirate_fid;
              hideout.system_id = chosen.system_id;
              hideout.design_id = "pirate_hideout";
              hideout.name = "Pirate Hideout " + std::to_string(hideout.id);
              hideout.sensor_mode = SensorMode::Passive;

              // Spawn a small random offset from the anchor (usually a jump point).
              const double ang = u01(rng) * kTwoPi;
              const double rad = 0.4 + u01(rng) * 1.6;
              hideout.position_mkm = anchor + Vec2{std::cos(ang), std::sin(ang)} * rad;

              state_.ships.emplace(hideout.id, hideout);
              state_.ship_orders.emplace(hideout.id, ShipOrders{});
              sys->ships.push_back(hideout.id);

              if (auto* sh = find_ptr(state_.ships, hideout.id)) {
                apply_design_stats_to_ship(*sh);
              }

              ++pirate_hideout_count;
            }
          }
        }
      }
    }

  }
}

void Simulation::tick_refuel() {
  NEBULA4X_TRACE_SCOPE("tick_refuel", "sim.maintenance");
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
    (void)sid;

    // Ambient/passive civilian ships (e.g. neutral merchant convoys) are
    // intentionally abstracted and do not pull supplies from colonies.
    if (const auto* fac = find_ptr(state_.factions, ship.faction_id);
        fac && fac->control == FactionControl::AI_Passive) {
      continue;
    }

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
      if (!are_factions_trade_partners(ship.faction_id, col->faction_id)) continue;

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



void Simulation::tick_rearm() {
  NEBULA4X_TRACE_SCOPE("tick_rearm", "sim.maintenance");
  constexpr const char* kMunitionsKey = "Munitions";

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
    (void)sid;

    // Ambient/passive civilian ships (e.g. neutral merchant convoys) are
    // intentionally abstracted and do not participate in the maintenance supply
    // loop. This avoids draining player stockpiles and keeps the civilian layer
    // lightweight.
    if (const auto* fac = find_ptr(state_.factions, ship.faction_id);
        fac && fac->control == FactionControl::AI_Passive) {
      continue;
    }

    const ShipDesign* d = find_design(ship.design_id);
    if (!d) continue;

    const int cap = std::max(0, d->missile_ammo_capacity);
    if (cap <= 0) continue;

    // Clamp away any weird negative sentinel states before using.
    if (ship.missile_ammo < 0) ship.missile_ammo = cap;
    ship.missile_ammo = std::clamp(ship.missile_ammo, 0, cap);

    int need = cap - ship.missile_ammo;
    if (need <= 0) continue;

    // First try to reload from ship-carried munitions (ammo tenders / cargo holds).
    if (need > 0) {
      auto cit = ship.cargo.find(kMunitionsKey);
      if (cit != ship.cargo.end()) {
        const double avail_d = std::max(0.0, cit->second);
        const int avail = static_cast<int>(std::floor(avail_d + 1e-9));
        const int take = std::min(need, avail);
        if (take > 0) {
          ship.missile_ammo += take;
          ship.missile_ammo = std::clamp(ship.missile_ammo, 0, cap);
          cit->second = avail_d - static_cast<double>(take);
          if (cit->second <= 1e-9) ship.cargo.erase(cit);
          need = cap - ship.missile_ammo;
        }
      }
    }
    if (need <= 0) continue;

    auto it = colonies_in_system.find(ship.system_id);
    if (it == colonies_in_system.end()) continue;

    Id best_cid = kInvalidId;
    double best_dist = 1e100;

    for (Id cid : it->second) {
      const Colony* col = find_ptr(state_.colonies, cid);
      if (!col) continue;
      if (!are_factions_trade_partners(ship.faction_id, col->faction_id)) continue;

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
    auto mit = col.minerals.find(kMunitionsKey);
    if (mit == col.minerals.end()) continue;

    const double avail_d = std::max(0.0, mit->second);
    if (avail_d < 1.0 - 1e-9) continue;

    const int avail = static_cast<int>(std::floor(avail_d + 1e-9));
    const int take = std::min(need, avail);
    if (take <= 0) continue;

    ship.missile_ammo += take;
    mit->second = avail_d - static_cast<double>(take);
    if (mit->second <= 1e-9) mit->second = 0.0;
  }
}


void Simulation::tick_ship_maintenance(double dt_days) {
  if (dt_days <= 0.0) return;
  if (!cfg_.enable_ship_maintenance) return;
  NEBULA4X_TRACE_SCOPE("tick_ship_maintenance", "sim.maintenance");

  const std::string& res = cfg_.ship_maintenance_resource_id;
  if (res.empty()) return;

  const double per_ton = std::max(0.0, cfg_.ship_maintenance_tons_per_day_per_mass_ton);
  const double rec = std::max(0.0, cfg_.ship_maintenance_recovery_per_day);
  const double dec = std::max(0.0, cfg_.ship_maintenance_decay_per_day);

  // If there is no consumption and no drift, nothing to do.
  if (per_ton <= 0.0 && rec <= 0.0 && dec <= 0.0) return;

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
    (void)sid;
    const ShipDesign* d = find_design(ship.design_id);
    if (!d) continue;

    // Ambient/passive civilian ships (e.g. neutral merchant convoys) are
    // intentionally abstracted and do not participate in the maintenance supply
    // loop. This avoids draining player stockpiles and keeps the civilian layer
    // lightweight.
    if (const auto* fac = find_ptr(state_.factions, ship.faction_id);
        fac && fac->control == FactionControl::AI_Passive) {
      continue;
    }

    // Sanitize in case older saves or mods produce out-of-range values.
    if (!std::isfinite(ship.maintenance_condition)) ship.maintenance_condition = 1.0;
    ship.maintenance_condition = std::clamp(ship.maintenance_condition, 0.0, 1.0);

    const double required = std::max(0.0, d->mass_tons) * per_ton * dt_days;
    double supplied = 0.0;
    double need = required;

    // Pull from ship cargo first (lets players bring spare parts on long deployments).
    if (need > 1e-9) {
      auto cit = ship.cargo.find(res);
      if (cit != ship.cargo.end()) {
        const double avail = std::max(0.0, cit->second);
        const double take = std::min(need, avail);
        if (take > 1e-9) {
          supplied += take;
          need -= take;
          cit->second = avail - take;
          if (cit->second <= 1e-9) cit->second = 0.0;
        }
      }
    }

    // If still short, pull from a nearby friendly colony stockpile.
    if (need > 1e-9) {
      auto it = colonies_in_system.find(ship.system_id);
      if (it != colonies_in_system.end()) {
        Id best_cid = kInvalidId;
        double best_avail = 0.0;
        double best_dist = 1e100;

        for (Id cid : it->second) {
          const Colony* col = find_ptr(state_.colonies, cid);
          if (!col) continue;
          if (!are_factions_trade_partners(ship.faction_id, col->faction_id)) continue;

          const Body* body = find_ptr(state_.bodies, col->body_id);
          if (!body) continue;
          const double dist = (body->position_mkm - ship.position_mkm).length();
          if (dist > dock_range + 1e-9) continue;

          const auto mit = col->minerals.find(res);
          const double avail = (mit == col->minerals.end()) ? 0.0 : std::max(0.0, mit->second);
          if (avail <= 1e-9) continue;

          // Prefer more available supplies, tiebreak on distance then id.
          if (avail > best_avail + 1e-9 ||
              (std::abs(avail - best_avail) <= 1e-9 && dist < best_dist - 1e-9) ||
              (std::abs(avail - best_avail) <= 1e-9 && std::abs(dist - best_dist) <= 1e-9 && cid < best_cid)) {
            best_avail = avail;
            best_dist = dist;
            best_cid = cid;
          }
        }

        if (best_cid != kInvalidId) {
          Colony& col = state_.colonies.at(best_cid);
          double& avail_ref = col.minerals[res];
          const double avail = std::max(0.0, avail_ref);
          const double take = std::min(need, avail);
          if (take > 1e-9) {
            supplied += take;
            need -= take;
            avail_ref = avail - take;
            if (avail_ref <= 1e-9) avail_ref = 0.0;
          }
        }
      }
    }

    // Update condition based on supply fraction.
    if (required > 1e-9) {
      const double frac = std::clamp(supplied / required, 0.0, 1.0);
      if (frac >= 1.0 - 1e-9) {
        if (rec > 0.0) ship.maintenance_condition = std::min(1.0, ship.maintenance_condition + rec * dt_days);
      } else {
        if (dec > 0.0) ship.maintenance_condition =
            std::max(0.0, ship.maintenance_condition - dec * (1.0 - frac) * dt_days);
      }
    } else if (rec > 0.0) {
      // No consumption configured; optionally allow condition to slowly recover.
      ship.maintenance_condition = std::min(1.0, ship.maintenance_condition + rec * dt_days);
    }
  }
}






void Simulation::tick_ship_maintenance_failures() {
  if (!cfg_.enable_ship_maintenance) return;
  NEBULA4X_TRACE_SCOPE("tick_ship_maintenance_failures", "sim.maintenance");

  const double start = std::clamp(cfg_.ship_maintenance_breakdown_start_fraction, 0.0, 1.0);
  const double rate0 = std::max(0.0, cfg_.ship_maintenance_breakdown_rate_per_day_at_zero);
  if (!(start > 1e-9) || !(rate0 > 1e-12)) return;

  const double exponent = std::max(0.1, cfg_.ship_maintenance_breakdown_exponent);

  double dmg_min = std::clamp(cfg_.ship_maintenance_breakdown_subsystem_damage_min, 0.0, 1.0);
  double dmg_max = std::clamp(cfg_.ship_maintenance_breakdown_subsystem_damage_max, 0.0, 1.0);
  if (dmg_max < dmg_min) std::swap(dmg_min, dmg_max);
  if (!(dmg_max > 1e-12)) return;

  const double dock_range = std::max(0.0, cfg_.docking_range_mkm);

  // Precompute shipyard-bearing colonies per system (used to suppress failures
  // while docked at a shipyard).
  std::unordered_map<Id, std::vector<Id>> shipyards_in_system;
  shipyards_in_system.reserve(state_.systems.size() * 2 + 8);

  for (const auto& [cid, col] : state_.colonies) {
    const auto it_y = col.installations.find("shipyard");
    const int yards = (it_y != col.installations.end()) ? it_y->second : 0;
    if (yards <= 0) continue;

    const Body* body = find_ptr(state_.bodies, col.body_id);
    if (!body) continue;

    shipyards_in_system[body->system_id].push_back(cid);
  }

  auto is_docked_at_shipyard = [&](const Ship& ship) -> bool {
    if (dock_range <= 1e-9) return false;
    auto it = shipyards_in_system.find(ship.system_id);
    if (it == shipyards_in_system.end()) return false;

    for (Id cid : it->second) {
      const Colony* col = find_ptr(state_.colonies, cid);
      if (!col) continue;
      if (!are_factions_trade_partners(ship.faction_id, col->faction_id)) continue;

      const Body* body = find_ptr(state_.bodies, col->body_id);
      if (!body) continue;

      const double dist = (ship.position_mkm - body->position_mkm).length();
      if (dist <= dock_range + 1e-9) return true;
    }
    return false;
  };

  const auto ship_ids = sorted_keys(state_.ships);
  const std::uint64_t day = static_cast<std::uint64_t>(state_.date.days_since_epoch());

  auto clamp01 = [](double x) -> double {
    if (!std::isfinite(x)) return 1.0;
    return std::clamp(x, 0.0, 1.0);
  };

  for (Id sid : ship_ids) {
    Ship* sh = find_ptr(state_.ships, sid);
    if (!sh) continue;
    if (sh->hp <= 0.0) continue;
    if (sh->system_id == kInvalidId) continue;

    const double cond = std::clamp(sh->maintenance_condition, 0.0, 1.0);
    if (cond >= start - 1e-9) continue;

    // Ships actively docked at a shipyard are assumed to have failures addressed.
    if (is_docked_at_shipyard(*sh)) continue;

    const double x = (start > 1e-9) ? std::clamp((start - cond) / start, 0.0, 1.0) : std::clamp(1.0 - cond, 0.0, 1.0);
    if (x <= 1e-9) continue;

    const double rate = rate0 * std::pow(x, exponent);
    const double p = 1.0 - std::exp(-rate);
    if (p <= 1e-12) continue;

    // Deterministic per-(ship,day) seed.
    std::uint64_t seed = static_cast<std::uint64_t>(sid) ^ (day * 0x9e3779b97f4a7c15ULL);

    if (u01(seed) >= p) continue;

    // Choose a subsystem that the design actually has.
    const ShipDesign* d = find_design(sh->design_id);

    struct Slot {
      const char* name;
      double* integrity;
    };
    std::vector<Slot> slots;
    slots.reserve(4);

    if (d) {
      if (d->speed_km_s > 1e-9) slots.push_back({"Engines", &sh->engines_integrity});

      const bool has_weapons = (d->weapon_damage > 1e-9) || (d->missile_damage > 1e-9);
      if (has_weapons) slots.push_back({"Weapons", &sh->weapons_integrity});

      if (d->sensor_range_mkm > 1e-9) slots.push_back({"Sensors", &sh->sensors_integrity});
      if (d->max_shields > 1e-9) slots.push_back({"Shields", &sh->shields_integrity});
    }

    if (slots.empty()) {
      // Fallback: treat as a generic failure affecting core systems.
      slots.push_back({"Systems", &sh->engines_integrity});
    }

    const int n = static_cast<int>(slots.size());
    const int idx = std::clamp(static_cast<int>(std::floor(u01(seed) * static_cast<double>(n))), 0, n - 1);

    // Damage scales up as maintenance gets worse.
    const double severity = std::clamp(0.35 + 0.65 * x, 0.0, 1.0);
    const double dmg = (dmg_min + (dmg_max - dmg_min) * u01(seed)) * severity;

    Slot& sl = slots[idx];
    const double before = clamp01(*sl.integrity);
    const double after = clamp01(before - dmg);
    *sl.integrity = after;

    // Also nudge maintenance_condition down slightly to reflect cascading issues.
    // (Keeps the sustainment loop "sticky" at very low readiness.)
    sh->maintenance_condition = std::clamp(cond - 0.01 * severity, 0.0, 1.0);

    if (is_player_faction(state_, sh->faction_id)) {
      EventContext ctx;
      ctx.faction_id = sh->faction_id;
      ctx.system_id = sh->system_id;
      ctx.ship_id = sh->id;

      const int pct = static_cast<int>(std::lround(after * 100.0));
      const int dpct = static_cast<int>(std::lround(std::max(0.0, before - after) * 100.0));

      std::ostringstream ss;
      ss << "Maintenance failure aboard " << sh->name << ": " << sl.name << " damaged (" << pct << "%, -" << dpct << "%)";

      const EventLevel lvl = (after <= 0.25) ? EventLevel::Warn : EventLevel::Info;
      push_event(lvl, EventCategory::Shipyard, ss.str(), ctx);
    }
  }
}


void Simulation::tick_crew_training(double dt_days) {
  if (dt_days <= 0.0) return;
  if (!cfg_.enable_crew_experience && !cfg_.enable_crew_casualties) return;
  NEBULA4X_TRACE_SCOPE("tick_crew_training", "sim.crew");

  const double dock_range = std::max(0.0, cfg_.docking_range_mkm);
  if (dock_range <= 0.0) return;

  // Deterministic processing order.
  const auto ship_ids = sorted_keys(state_.ships);
  const auto colony_ids = sorted_keys(state_.colonies);

  for (Id cid : colony_ids) {
    Colony* col = find_ptr(state_.colonies, cid);
    if (!col) continue;
    const Body* body = find_ptr(state_.bodies, col->body_id);
    if (!body) continue;

    const double pool_per_day = crew_training_points_per_day(*col);
    if (pool_per_day <= 1e-9) continue;

    std::vector<Id> docked;
    docked.reserve(8);
    for (Id sid : ship_ids) {
      const Ship* sh = find_ptr(state_.ships, sid);
      if (!sh) continue;
      if (sh->system_id != body->system_id) continue;
      if (sh->faction_id != col->faction_id) continue;
      const double dist = (sh->position_mkm - body->position_mkm).length();
      if (dist > dock_range + 1e-9) continue;
      docked.push_back(sid);
    }

    if (docked.empty()) continue;

    const double per_ship = (pool_per_day / static_cast<double>(docked.size())) * dt_days;
    if (per_ship <= 1e-12) continue;

    const double cap = std::max(0.0, cfg_.crew_grade_points_cap);
    const double rep_points_full = std::max(0.0, cfg_.crew_replacement_training_points_per_full_complement);
    for (Id sid : docked) {
      Ship& sh = state_.ships.at(sid);

      // Normalize legacy / modded state.
      if (!std::isfinite(sh.crew_grade_points) || sh.crew_grade_points < 0.0) {
        sh.crew_grade_points = cfg_.crew_initial_grade_points;
      }
      sh.crew_grade_points = std::max(0.0, sh.crew_grade_points);

      if (!std::isfinite(sh.crew_complement) || sh.crew_complement < 0.0) sh.crew_complement = 1.0;
      sh.crew_complement = std::clamp(sh.crew_complement, 0.0, 1.0);

      double points = per_ship;

      // Crew replacement draws from the same pool as training.
      if (cfg_.enable_crew_casualties && rep_points_full > 1e-9 && sh.crew_complement + 1e-12 < 1.0 && points > 1e-12) {
        const double comp_before = sh.crew_complement;
        const double missing = std::clamp(1.0 - comp_before, 0.0, 1.0);
        const double need = missing * rep_points_full;
        const double use = std::min(points, need);
        const double delta = use / rep_points_full;
        const double comp_after = std::clamp(comp_before + delta, 0.0, 1.0);
        sh.crew_complement = comp_after;
        points -= use;

        // Dilute average grade points by mixing in green replacements.
        const double delta_c = comp_after - comp_before;
        if (delta_c > 1e-12 && comp_after > 1e-12) {
          const double gp0 = cfg_.crew_initial_grade_points;
          const double gp_before = sh.crew_grade_points;
          const double gp_after = (gp_before * comp_before + gp0 * delta_c) / comp_after;
          sh.crew_grade_points = std::max(0.0, gp_after);
        }
      }

      // Remaining points go to training if enabled.
      if (cfg_.enable_crew_experience && points > 1e-12) {
        sh.crew_grade_points += points;
        if (cap > 0.0) sh.crew_grade_points = std::min(cap, sh.crew_grade_points);
      }
    }
  }
}




void Simulation::tick_repairs(double dt_days) {
  if (dt_days <= 0.0) return;
  NEBULA4X_TRACE_SCOPE("tick_repairs", "sim.maintenance");
  const double per_yard = std::max(0.0, cfg_.repair_hp_per_day_per_shipyard);
  if (per_yard <= 0.0) return;

  const double dock_range = std::max(0.0, cfg_.docking_range_mkm);

  const double cost_dur = std::max(0.0, cfg_.repair_duranium_per_hp);
  const double cost_neu = std::max(0.0, cfg_.repair_neutronium_per_hp);

  const double subsys_hp_equiv_per_integrity = std::max(0.0, cfg_.ship_subsystem_repair_hp_equiv_per_integrity);
  const bool subsys_repairs_enabled = subsys_hp_equiv_per_integrity > 1e-12;

  auto clamp01 = [](double x) -> double {
    if (!std::isfinite(x)) return 1.0;
    return std::clamp(x, 0.0, 1.0);
  };

  auto ship_subsys_deficit_points = [&](const Ship& s) -> double {
    if (!subsys_repairs_enabled) return 0.0;
    const double e = clamp01(s.engines_integrity);
    const double w = clamp01(s.weapons_integrity);
    const double se = clamp01(s.sensors_integrity);
    const double sh = clamp01(s.shields_integrity);
    return std::max(0.0, 1.0 - e) + std::max(0.0, 1.0 - w) + std::max(0.0, 1.0 - se) + std::max(0.0, 1.0 - sh);
  };

  auto ship_subsys_deficit_hp_equiv = [&](const Ship& s, double max_hp) -> double {
    if (!subsys_repairs_enabled) return 0.0;
    if (!(max_hp > 1e-12)) return 0.0;
    return ship_subsys_deficit_points(s) * max_hp * subsys_hp_equiv_per_integrity;
  };

  // Assign each damaged ship to the *single* best docked shipyard colony (most yards, then closest).
  // This avoids a ship being repaired multiple times in one tick when multiple colonies are within docking range.
  std::unordered_map<Id, std::vector<Id>> ships_by_colony;
  ships_by_colony.reserve(state_.colonies.size() * 2);

  const auto ship_ids = sorted_keys(state_.ships);
  const auto colony_ids = sorted_keys(state_.colonies);

  for (Id sid : ship_ids) {
    auto* ship = find_ptr(state_.ships, sid);
    if (!ship) continue;

    // Ambient/passive civilian ships are abstracted and do not use faction
    // shipyards or colony resources for repairs.
    if (const auto* fac = find_ptr(state_.factions, ship->faction_id);
        fac && fac->control == FactionControl::AI_Passive) {
      continue;
    }

    const auto* d = find_design(ship->design_id);
    const double max_hp = d ? d->max_hp : ship->hp;
    if (max_hp <= 0.0) continue;

    // Clamp just in case something drifted out of bounds (custom content, legacy saves, etc.).
    ship->hp = std::clamp(ship->hp, 0.0, max_hp);

    // Clamp subsystem integrity even if repairs are disabled; it keeps things sane for future enabling.
    ship->engines_integrity = clamp01(ship->engines_integrity);
    ship->weapons_integrity = clamp01(ship->weapons_integrity);
    ship->sensors_integrity = clamp01(ship->sensors_integrity);
    ship->shields_integrity = clamp01(ship->shields_integrity);

    const bool needs_hull = ship->hp < max_hp - 1e-9;
    const bool needs_subsys = ship_subsys_deficit_hp_equiv(*ship, max_hp) > 1e-9;

    if (!needs_hull && !needs_subsys) continue;

    Id best_colony = kInvalidId;
    int best_shipyards = 0;
    double best_dist = 0.0;

    for (Id cid : colony_ids) {
      const auto* colony = find_ptr(state_.colonies, cid);
      if (!colony) continue;
      if (!are_factions_trade_partners(ship->faction_id, colony->faction_id)) continue;

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
    if (cfg_.enable_blockades) capacity *= blockade_output_multiplier_for_colony(cid);
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
    double applied_total_equiv = 0.0;

    for (Id sid : list) {
      if (remaining <= 1e-9) break;

      auto* ship = find_ptr(state_.ships, sid);
      if (!ship) continue;

      const auto* d = find_design(ship->design_id);
      const double max_hp = d ? d->max_hp : ship->hp;
      if (max_hp <= 0.0) continue;

      ship->hp = std::clamp(ship->hp, 0.0, max_hp);

      // Clamp subsystem integrity to keep repair math stable.
      ship->engines_integrity = clamp01(ship->engines_integrity);
      ship->weapons_integrity = clamp01(ship->weapons_integrity);
      ship->sensors_integrity = clamp01(ship->sensors_integrity);
      ship->shields_integrity = clamp01(ship->shields_integrity);

      const double hull_missing = std::max(0.0, max_hp - ship->hp);
      const double subsys_def_pts_before = ship_subsys_deficit_points(*ship);
      const double subsys_missing_equiv = ship_subsys_deficit_hp_equiv(*ship, max_hp);

      const double total_missing_equiv = hull_missing + subsys_missing_equiv;
      if (total_missing_equiv <= 1e-9) continue;

      const double hp_before = ship->hp;

      const double apply_total = std::min(remaining, total_missing_equiv);

      // Repair hull first.
      const double apply_hull = std::min(apply_total, hull_missing);
      if (apply_hull > 0.0) ship->hp = std::min(max_hp, ship->hp + apply_hull);

      // Then apply any remaining capacity to subsystem integrity.
      double restored_subsys_points = 0.0;
      const double apply_left_equiv = apply_total - apply_hull;
      if (subsys_repairs_enabled && apply_left_equiv > 1e-9 && max_hp > 1e-9) {
        double points = apply_left_equiv / (max_hp * subsys_hp_equiv_per_integrity);
        if (points > 1e-12) {
          struct Slot {
            const char* name;
            double* integrity;
          };
          std::vector<Slot> slots = {{"Engines", &ship->engines_integrity},
                                     {"Weapons", &ship->weapons_integrity},
                                     {"Sensors", &ship->sensors_integrity},
                                     {"Shields", &ship->shields_integrity}};

          // Prioritize the most damaged subsystem(s) first.
          std::sort(slots.begin(), slots.end(), [&](const Slot& a, const Slot& b) {
            const double ia = clamp01(*a.integrity);
            const double ib = clamp01(*b.integrity);
            if (ia != ib) return ia < ib;
            return std::strcmp(a.name, b.name) < 0;
          });

          for (auto& sl : slots) {
            if (points <= 1e-12) break;
            double cur = clamp01(*sl.integrity);
            const double missing = std::max(0.0, 1.0 - cur);
            if (missing <= 1e-12) {
              *sl.integrity = cur;
              continue;
            }
            const double restore = std::min(missing, points);
            cur = std::clamp(cur + restore, 0.0, 1.0);
            *sl.integrity = cur;
            points -= restore;
            restored_subsys_points += restore;
          }
        }
      }

      const double subsys_equiv_used = restored_subsys_points * max_hp * subsys_hp_equiv_per_integrity;
      const double applied_equiv = std::max(0.0, (ship->hp - hp_before)) + std::max(0.0, subsys_equiv_used);

      if (applied_equiv <= 1e-12) continue;

      remaining -= applied_equiv;
      applied_total_equiv += applied_equiv;

      const double subsys_def_pts_after = ship_subsys_deficit_points(*ship);
      const bool fully_repaired =
          (ship->hp >= max_hp - 1e-9) && (!subsys_repairs_enabled || subsys_def_pts_after <= 1e-9);

      const bool was_damaged = (hp_before < max_hp - 1e-9) || (subsys_def_pts_before > 1e-9);
      if (was_damaged && fully_repaired) {
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

    if (applied_total_equiv <= 1e-9) continue;

    // Consume repair minerals (HP-equivalent: hull HP + subsystem integrity repairs).
    if (cost_dur > 1e-12) {
      double& dur = colony->minerals["Duranium"];
      dur = std::max(0.0, dur - applied_total_equiv * cost_dur);
    }
    if (cost_neu > 1e-12) {
      double& neu = colony->minerals["Neutronium"];
      neu = std::max(0.0, neu - applied_total_equiv * cost_neu);
    }
  }
}


} // namespace nebula4x
