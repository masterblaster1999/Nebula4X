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
#include "nebula4x/core/troop_planner.h"
#include "nebula4x/util/log.h"
#include "nebula4x/util/trace_events.h"
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
    const auto* sys = find_ptr(state_.systems, ship->system_id);
    if (!sys) return false;

    const auto it_cache = explore_cache.find(fid);
    const ExploreFactionCache* cache = (it_cache != explore_cache.end()) ? &it_cache->second : nullptr;

    auto& reserved_jumps = reserved_explore_jump_targets[fid];
    auto& reserved_frontiers = reserved_explore_frontier_targets[fid];

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
      issue_move_to_point(ship_id, best_survey_pos);
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
      double used = 0.0;
      for (const auto& [_, tons] : s.cargo) used += std::max(0.0, tons);
      return used;
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
    if (sh->auto_mine) continue;     // mutually exclusive; auto-mine handled above
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



  // --- Fleet missions (player automation) ---
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
      return std::clamp(hp / max_hp, 0.0, 1.0);
    };

    auto select_refuel_colony_for_fleet = [&](Id fleet_faction_id, Id start_sys, Vec2 start_pos, double speed_km_s) -> Id {
      if (speed_km_s <= 0.0) return kInvalidId;

      Id best_cid = kInvalidId;
      double best_eta = std::numeric_limits<double>::infinity();
      bool best_has_fuel = false;

      for (Id cid : sorted_keys(state_.colonies)) {
        const Colony* c = find_ptr(state_.colonies, cid);
        if (!c) continue;
        if (!are_factions_mutual_friendly(fleet_faction_id, c->faction_id)) continue;
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
        if (!are_factions_mutual_friendly(fl.faction_id, c->faction_id)) continue;

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

    auto combat_target_priority = [&](ShipRole r) -> int {
      // For player-side missions we bias toward removing armed threats first.
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
      if (!fac || fac->control != FactionControl::Player) continue;

      Ship* leader = pick_fleet_leader(fl);
      if (!leader) continue;

      const double fleet_speed = fleet_min_speed_km_s(fl, leader->speed_km_s);

      // --- Sustainment (refuel/repair) ---
      const double refuel_thr = std::clamp(fl.mission.refuel_threshold_fraction, 0.0, 1.0);
      const double refuel_resume = std::clamp(fl.mission.refuel_resume_fraction, 0.0, 1.0);
      const double repair_thr = std::clamp(fl.mission.repair_threshold_fraction, 0.0, 1.0);
      const double repair_resume = std::clamp(fl.mission.repair_resume_fraction, 0.0, 1.0);

      bool any_need_refuel = false;
      bool all_refueled = true;
      bool any_need_repair = false;
      bool all_repaired = true;

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
      }

      if (!fl.mission.auto_refuel) {
        any_need_refuel = false;
        all_refueled = true;
      }
      if (!fl.mission.auto_repair) {
        any_need_repair = false;
        all_repaired = true;
      }

      // Sustainment state transitions.
      if (fl.mission.sustainment_mode == FleetSustainmentMode::Refuel && all_refueled) {
        fl.mission.sustainment_mode = FleetSustainmentMode::None;
        fl.mission.sustainment_colony_id = kInvalidId;
      }
      if (fl.mission.sustainment_mode == FleetSustainmentMode::Repair && all_repaired) {
        fl.mission.sustainment_mode = FleetSustainmentMode::None;
        fl.mission.sustainment_colony_id = kInvalidId;
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
        }
      }

      if (fl.mission.sustainment_mode != FleetSustainmentMode::None) {
        // Maintain or acquire a sustainment dock.
        const Id cid = fl.mission.sustainment_colony_id;
        const Colony* col = find_ptr(state_.colonies, cid);
        const Body* body = col ? find_ptr(state_.bodies, col->body_id) : nullptr;
        const Id sys_id = body ? body->system_id : kInvalidId;

        if (!col || !body || sys_id == kInvalidId || !are_factions_mutual_friendly(fl.faction_id, col->faction_id) ||
            !is_system_discovered_by_faction(fl.faction_id, sys_id)) {
          // Can't sustain here; fall back to no sustainment.
          fl.mission.sustainment_mode = FleetSustainmentMode::None;
          fl.mission.sustainment_colony_id = kInvalidId;
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
          if (!are_factions_mutual_friendly(fl.faction_id, c->faction_id)) continue;
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

        std::vector<Id> jps = sys->jump_points;
        std::sort(jps.begin(), jps.end());

        const bool survey_first = fl.mission.explore_survey_first;
        const bool allow_transit = fl.mission.explore_allow_transit;

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

        auto issue_survey = [&](Id jp_id) {
          if (jp_id == kInvalidId) return;
          const JumpPoint* jp = find_ptr(state_.jump_points, jp_id);
          if (!jp) return;
          reserved_jumps.insert(jp_id);
          clear_fleet_orders(fid);
          issue_fleet_move_to_point(fid, jp->position_mkm);
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
            issue_survey(survey_jump);
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
            issue_survey(survey_jump);
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
          if (!are_factions_mutual_friendly(fl.faction_id, c->faction_id)) continue;
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
  NEBULA4X_TRACE_SCOPE("tick_repairs", "sim.maintenance");
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
