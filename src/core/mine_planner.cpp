#include "nebula4x/core/mine_planner.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "nebula4x/core/simulation.h"

namespace nebula4x {
namespace {

double cargo_used_tons(const Ship& sh) {
  double used = 0.0;
  for (const auto& [_, tons] : sh.cargo) {
    used += tons;
  }
  return used;
}

double positive_sum(const std::unordered_map<std::string, double>& m) {
  double s = 0.0;
  for (const auto& [_, v] : m) {
    if (v > 0.0) s += v;
  }
  return s;
}

double deposit_tons_for_mineral(const Body& b, const std::string& mineral) {
  // Empty deposits means "unknown / unlimited" in some legacy setups.
  if (b.mineral_deposits.empty()) return 1e30;

  if (mineral.empty()) {
    return positive_sum(b.mineral_deposits);
  }

  const auto it = b.mineral_deposits.find(mineral);
  if (it == b.mineral_deposits.end()) return 0.0;
  return std::max(0.0, it->second);
}

std::string best_missing_mineral(const std::unordered_map<std::string, double>& missing) {
  std::string best;
  double best_missing = 0.0;
  for (const auto& [name, tons] : missing) {
    if (!(tons > 0.0)) continue;
    if (best.empty() || tons > best_missing || (tons == best_missing && name < best)) {
      best = name;
      best_missing = tons;
    }
  }
  return best;
}

}  // namespace

MinePlannerResult compute_mine_plan(const Simulation& sim, Id faction_id, const MinePlannerOptions& opt) {
  MinePlannerResult out;

  const auto& st = sim.state();
  const Faction* fac = find_ptr(st.factions, faction_id);
  if (!fac) {
    out.message = "Invalid faction id.";
    return out;
  }

  const double min_tons = (opt.min_tons > 0.0) ? std::max(1e-6, opt.min_tons)
                                               : std::max(1e-6, sim.cfg().auto_freight_min_transfer_tons);

  // --- Owned colonies (potential unload destinations).
  std::vector<Id> colony_ids;
  colony_ids.reserve(st.colonies.size());
  for (const auto& [cid, c] : st.colonies) {
    if (cid == kInvalidId) continue;
    if (c.faction_id != faction_id) continue;
    colony_ids.push_back(cid);
  }
  std::sort(colony_ids.begin(), colony_ids.end());

  // --- Mineral shortages (used for smarter mineral selection and delivery).
  //
  // The logistics system reports many kinds of needs (fuel, shipyards, industry
  // inputs, etc.). Mobile miners can only directly provide *mineable resources*
  // (whatever exists as a mineral deposit key on bodies), so we focus on the
  // mineral-backed needs that are meaningful to satisfy with mining.
  std::unordered_map<Id, std::unordered_map<std::string, double>> missing_by_colony;
  std::unordered_map<std::string, double> missing_total;
  for (const auto& need : sim.logistics_needs_for_faction(faction_id)) {
    if (!(need.missing_tons > 0.0)) continue;

    const bool relevant = (need.kind == LogisticsNeedKind::StockpileTarget) ||
                          (need.kind == LogisticsNeedKind::Shipyard) ||
                          (need.kind == LogisticsNeedKind::Construction) ||
                          (need.kind == LogisticsNeedKind::IndustryInput) ||
                          (need.kind == LogisticsNeedKind::TroopTraining) ||
                          (need.kind == LogisticsNeedKind::Fuel);
    if (!relevant) continue;

    missing_by_colony[need.colony_id][need.mineral] += need.missing_tons;
    missing_total[need.mineral] += need.missing_tons;
  }

  // --- Reserved bodies (existing orders + caller-provided).
  std::unordered_set<Id> reserved_bodies;
  reserved_bodies.reserve(opt.reserved_body_ids.size() + 256);
  for (Id bid : opt.reserved_body_ids) {
    if (bid == kInvalidId) continue;
    reserved_bodies.insert(bid);
  }

  if (opt.reserve_bodies_targeted_by_existing_orders) {
    auto reserve_order = [&](const Order& ord) {
      if (const auto* mb = std::get_if<MineBody>(&ord)) {
        if (mb->body_id != kInvalidId) reserved_bodies.insert(mb->body_id);
      }
    };

    for (const auto& [sid, so] : st.ship_orders) {
      if (sid == kInvalidId) continue;
      const Ship* sh = find_ptr(st.ships, sid);
      if (!sh || sh->faction_id != faction_id) continue;

      for (const auto& ord : so.queue) reserve_order(ord);
      if (so.repeat) {
        for (const auto& ord : so.repeat_template) reserve_order(ord);
      }
      if (so.suspended) {
        for (const auto& ord : so.suspended_queue) reserve_order(ord);
        if (so.suspended_repeat) {
          for (const auto& ord : so.suspended_repeat_template) reserve_order(ord);
        }
      }
    }
  }

  // --- Candidate mining bodies.
  struct CandidateBody {
    Id id{kInvalidId};
    Id system_id{kInvalidId};
    Vec2 pos_mkm{0.0, 0.0};
    BodyType type{BodyType::Planet};
  };

  std::vector<Id> body_ids;
  body_ids.reserve(st.bodies.size());
  for (const auto& [bid, _] : st.bodies) {
    if (bid == kInvalidId) continue;
    body_ids.push_back(bid);
  }
  std::sort(body_ids.begin(), body_ids.end());

  std::vector<CandidateBody> bodies;
  bodies.reserve(std::min<std::size_t>(body_ids.size(), static_cast<std::size_t>(opt.max_bodies)));

  for (Id bid : body_ids) {
    const Body* b = find_ptr(st.bodies, bid);
    if (!b) continue;
    if (b->system_id == kInvalidId) continue;
    if (!find_ptr(st.systems, b->system_id)) continue;
    if (b->type == BodyType::Star) continue;
    if (reserved_bodies.contains(bid)) continue;

    if (opt.restrict_to_discovered && !sim.is_system_discovered_by_faction(faction_id, b->system_id)) {
      continue;
    }

    if (opt.avoid_hostile_systems && !sim.detected_hostile_ships_in_system(faction_id, b->system_id).empty()) {
      continue;
    }

    bool mineable = false;
    if (b->mineral_deposits.empty()) {
      mineable = true;
    } else {
      for (const auto& [_, tons] : b->mineral_deposits) {
        if (tons > 1e-9) {
          mineable = true;
          break;
        }
      }
    }
    if (!mineable) continue;

    bodies.push_back(CandidateBody{.id = bid, .system_id = b->system_id, .pos_mkm = b->position_mkm, .type = b->type});
    if (static_cast<int>(bodies.size()) >= opt.max_bodies) {
      out.truncated = true;
      break;
    }
  }

  // --- Candidate mining ships.
  std::vector<Id> ship_ids;
  ship_ids.reserve(st.ships.size());
  for (const auto& [sid, _] : st.ships) {
    if (sid == kInvalidId) continue;
    ship_ids.push_back(sid);
  }
  std::sort(ship_ids.begin(), ship_ids.end());

  struct CandidateShip {
    Id id{kInvalidId};
    const Ship* sh{nullptr};
    const ShipDesign* design{nullptr};
  };

  std::vector<CandidateShip> ships;
  ships.reserve(std::min<std::size_t>(ship_ids.size(), static_cast<std::size_t>(opt.max_ships)));

  for (Id sid : ship_ids) {
    const Ship* sh = find_ptr(st.ships, sid);
    if (!sh) continue;
    if (sh->faction_id != faction_id) continue;

    if (opt.require_auto_mine_flag && !sh->auto_mine) continue;

    if (opt.exclude_conflicting_automation_flags) {
      if (sh->auto_salvage) continue;
      if (sh->auto_freight) continue;
      if (sh->auto_explore) continue;
      if (sh->auto_colonize) continue;
      if (sh->auto_tanker) continue;
    }

    const ShipDesign* design = sim.find_design(sh->design_id);
    if (!design) continue;
    if (!(design->mining_tons_per_day > 0.0)) continue;
    if (!(design->cargo_tons >= min_tons)) continue;

    const ShipOrders* so = find_ptr(st.ship_orders, sid);
    if (opt.require_idle && so && !ship_orders_is_idle_for_automation(*so)) {
      continue;
    }

    if (opt.exclude_fleet_ships && sim.fleet_for_ship(sid) != kInvalidId) {
      continue;
    }

    ships.push_back(CandidateShip{.id = sid, .sh = sh, .design = design});
    if (static_cast<int>(ships.size()) >= opt.max_ships) {
      out.truncated = true;
      break;
    }
  }

  // No mining ships - return success, but empty plan.
  if (ships.empty()) {
    out.ok = true;
    out.message = "No eligible mining ships.";
    return out;
  }

  // ETA helper.
  auto estimate_eta_days = [&](Id from_system, const Vec2& from_pos_mkm, double speed_km_s, Id to_system,
                               const Vec2& to_pos_mkm) -> double {
    if (!(speed_km_s > 0.0)) {
      // No mobility - only allow zero-distance moves.
      if (from_system == to_system && from_pos_mkm == to_pos_mkm) return 0.0;
      return std::numeric_limits<double>::infinity();
    }

    const auto route = sim.plan_jump_route_from_pos(from_system, from_pos_mkm, faction_id, speed_km_s, to_system,
                                                   opt.restrict_to_discovered, to_pos_mkm);
    if (!route) return std::numeric_limits<double>::infinity();
    return route->total_eta_days;
  };

  // --- Assignments.
  out.assignments.reserve(ships.size());

  for (const auto& cs : ships) {
    const Ship& ship = *cs.sh;
    const ShipDesign& design = *cs.design;

    const double cargo_used = cargo_used_tons(ship);
    const double cargo_free = std::max(0.0, design.cargo_tons - cargo_used);

    // Determine a valid home colony if configured.
    Id home_colony = kInvalidId;
    if (ship.auto_mine_home_colony_id != kInvalidId) {
      const Colony* hc = find_ptr(st.colonies, ship.auto_mine_home_colony_id);
      if (hc && hc->faction_id == faction_id) {
        home_colony = ship.auto_mine_home_colony_id;
      }
    }

    // Helper to get a colony's position.
    auto colony_pos = [&](Id cid, Id* out_system) -> Vec2 {
      const Colony* c = find_ptr(st.colonies, cid);
      if (!c) {
        if (out_system) *out_system = kInvalidId;
        return Vec2{0.0, 0.0};
      }
      const Body* b = find_ptr(st.bodies, c->body_id);
      if (!b) {
        if (out_system) *out_system = kInvalidId;
        return Vec2{0.0, 0.0};
      }
      if (out_system) *out_system = b->system_id;
      return b->position_mkm;
    };

    // Helper to pick nearest colony (by ETA) among a subset.
    auto pick_nearest_colony = [&](const std::vector<Id>& candidates) -> Id {
      Id best = kInvalidId;
      double best_eta = std::numeric_limits<double>::infinity();
      for (Id cid : candidates) {
        Id sys = kInvalidId;
        const Vec2 pos = colony_pos(cid, &sys);
        if (sys == kInvalidId) continue;
        const double eta = estimate_eta_days(ship.system_id, ship.position_mkm, ship.speed_km_s, sys, pos);
        if (!(eta < best_eta)) continue;
        best_eta = eta;
        best = cid;
      }
      return best;
    };

    // If ship already has cargo, prioritize unloading it.
    if (cargo_used > 1e-6) {
      if (colony_ids.empty()) continue;

      Id dest = home_colony;
      if (dest == kInvalidId) {
        dest = pick_nearest_colony(colony_ids);
      }
      if (dest == kInvalidId) continue;

      MineAssignment asg;
      asg.kind = MineAssignmentKind::DeliverCargo;
      asg.ship_id = ship.id;
      asg.dest_colony_id = dest;

      Id dest_sys = kInvalidId;
      const Vec2 dest_pos = colony_pos(dest, &dest_sys);
      asg.eta_to_dest_days = (dest_sys != kInvalidId)
                                 ? estimate_eta_days(ship.system_id, ship.position_mkm, ship.speed_km_s, dest_sys, dest_pos)
                                 : std::numeric_limits<double>::infinity();
      asg.eta_total_days = asg.eta_to_dest_days;
      asg.expected_mined_tons = 0.0;
      asg.note = "Unload existing cargo.";

      out.assignments.push_back(std::move(asg));
      continue;
    }

    // Determine what mineral this ship should mine.
    std::string desired_mineral = ship.auto_mine_mineral;

    // If not specified, try to mine what the home colony needs most.
    if (desired_mineral.empty() && home_colony != kInvalidId) {
      const auto it = missing_by_colony.find(home_colony);
      if (it != missing_by_colony.end()) {
        desired_mineral = best_missing_mineral(it->second);
      }
    }

    // If still not specified, mine what the faction needs most.
    if (desired_mineral.empty()) {
      desired_mineral = best_missing_mineral(missing_total);
    }

    // Choose destination colony.
    Id dest_colony = kInvalidId;
    if (!colony_ids.empty()) {
      if (home_colony != kInvalidId) {
        dest_colony = home_colony;
      } else if (!desired_mineral.empty()) {
        // Prefer a colony that needs this mineral.
        std::vector<Id> needy;
        needy.reserve(colony_ids.size());
        for (Id cid : colony_ids) {
          const auto itc = missing_by_colony.find(cid);
          if (itc == missing_by_colony.end()) continue;
          const auto itm = itc->second.find(desired_mineral);
          if (itm != itc->second.end() && itm->second > 0.0) {
            needy.push_back(cid);
          }
        }
        dest_colony = needy.empty() ? pick_nearest_colony(colony_ids) : pick_nearest_colony(needy);
      } else {
        dest_colony = pick_nearest_colony(colony_ids);
      }
    }

    Id dest_sys = kInvalidId;
    Vec2 dest_pos{0.0, 0.0};
    if (dest_colony != kInvalidId) {
      dest_pos = colony_pos(dest_colony, &dest_sys);
    }

    // Select best mining body.
    Id best_body = kInvalidId;
    double best_score = -1.0;
    double best_eta_to_mine = 0.0;
    double best_eta_to_dest = 0.0;
    double best_deposit = 0.0;

    for (const auto& cb : bodies) {
      if (reserved_bodies.contains(cb.id)) continue;
      const Body* b = find_ptr(st.bodies, cb.id);
      if (!b) continue;

      const double deposit_tons = deposit_tons_for_mineral(*b, desired_mineral);
      if (!(deposit_tons > 1e-9)) continue;

      // Avoid tiny deposits that would never be worth traveling for.
      if (deposit_tons < min_tons) continue;

      const double expected = std::min(deposit_tons, cargo_free);
      if (!(expected > 1e-9)) continue;

      const double mine_rate = std::max(1e-6, design.mining_tons_per_day);
      const double mine_days = expected / mine_rate;

      const double eta_to_mine = estimate_eta_days(ship.system_id, ship.position_mkm, ship.speed_km_s, cb.system_id, cb.pos_mkm);
      if (!std::isfinite(eta_to_mine)) continue;

      double eta_to_dest = 0.0;
      if (dest_sys != kInvalidId) {
        eta_to_dest = estimate_eta_days(cb.system_id, cb.pos_mkm, ship.speed_km_s, dest_sys, dest_pos);
        if (!std::isfinite(eta_to_dest)) continue;
      }

      const double total_days = eta_to_mine + mine_days + eta_to_dest;
      const double denom = std::max(0.25, total_days);
      double score = expected / denom;

      // Mild bias toward asteroids/comets (common mobile mining targets).
      if (cb.type == BodyType::Asteroid || cb.type == BodyType::Comet) {
        score *= 1.25;
      }

      const bool better = (score > best_score) ||
                          (score == best_score && total_days < (best_eta_to_mine + mine_days + best_eta_to_dest)) ||
                          (score == best_score && total_days == (best_eta_to_mine + mine_days + best_eta_to_dest) &&
                           deposit_tons > best_deposit) ||
                          (score == best_score && total_days == (best_eta_to_mine + mine_days + best_eta_to_dest) &&
                           deposit_tons == best_deposit && cb.id < best_body);

      if (!better) continue;

      best_score = score;
      best_body = cb.id;
      best_eta_to_mine = eta_to_mine;
      best_eta_to_dest = eta_to_dest;
      best_deposit = deposit_tons;
    }

    if (best_body == kInvalidId) {
      continue;
    }

    reserved_bodies.insert(best_body);

    MineAssignment asg;
    asg.kind = MineAssignmentKind::MineAndDeliver;
    asg.ship_id = ship.id;
    asg.body_id = best_body;
    asg.dest_colony_id = dest_colony;
    asg.mineral = desired_mineral;
    asg.stop_when_cargo_full = true;

    const Body* bb = find_ptr(st.bodies, best_body);
    asg.deposit_tons = bb ? deposit_tons_for_mineral(*bb, desired_mineral) : 0.0;
    asg.expected_mined_tons = std::min(asg.deposit_tons, cargo_free);
    asg.mine_tons_per_day = design.mining_tons_per_day;
    asg.est_mine_days = (asg.mine_tons_per_day > 0.0) ? (asg.expected_mined_tons / std::max(1e-6, asg.mine_tons_per_day))
                                                      : std::numeric_limits<double>::infinity();

    asg.eta_to_mine_days = best_eta_to_mine;
    asg.eta_to_dest_days = best_eta_to_dest;
    asg.eta_total_days = asg.eta_to_mine_days + asg.est_mine_days + asg.eta_to_dest_days;

    if (desired_mineral.empty()) {
      asg.note = "Mine (all minerals).";
    } else {
      asg.note = "Mine " + desired_mineral + ".";
    }

    out.assignments.push_back(std::move(asg));
  }

  out.ok = true;
  if (out.assignments.empty()) {
    out.message = "No mine assignments produced.";
  } else {
    out.message = "Mine plan ready.";
  }
  return out;
}

bool apply_mine_assignment(Simulation& sim, const MineAssignment& asg, bool clear_existing_orders) {
  if (asg.ship_id == kInvalidId) return false;

  // Safety: don't generate orders for invalid entities.
  if (!find_ptr(sim.state().ships, asg.ship_id)) return false;

  if (clear_existing_orders) {
    sim.clear_orders(asg.ship_id);
  }

  switch (asg.kind) {
    case MineAssignmentKind::DeliverCargo: {
      if (asg.dest_colony_id == kInvalidId) return false;
      return sim.issue_unload_mineral(asg.ship_id, asg.dest_colony_id, /*mineral=*/"", /*tons=*/0.0);
    }

    case MineAssignmentKind::MineAndDeliver: {
      if (asg.body_id == kInvalidId) return false;
      const bool ok_mine = sim.issue_mine_body(asg.ship_id, asg.body_id, asg.mineral, asg.stop_when_cargo_full,
                                              /*restrict_to_discovered=*/true);
      if (!ok_mine) return false;

      if (asg.dest_colony_id != kInvalidId) {
        (void)sim.issue_unload_mineral(asg.ship_id, asg.dest_colony_id, /*mineral=*/"", /*tons=*/0.0);
      }
      return true;
    }
  }

  return false;
}

bool apply_mine_plan(Simulation& sim, const MinePlannerResult& plan, bool clear_existing_orders) {
  if (!plan.ok) return false;

  bool all_ok = true;
  for (const auto& asg : plan.assignments) {
    const bool ok = apply_mine_assignment(sim, asg, clear_existing_orders);
    all_ok = all_ok && ok;
  }
  return all_ok;
}

}  // namespace nebula4x
