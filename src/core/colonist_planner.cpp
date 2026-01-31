#include "nebula4x/core/colonist_planner.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "nebula4x/core/simulation.h"

namespace nebula4x {
namespace {

constexpr double kEps = 1e-9;

struct ColonyPopInfo {
  double target{0.0};
  double reserve{0.0};
  double current{0.0};
  double floor{0.0};
  double deficit{0.0};
  double surplus{0.0};
  std::string reason;
};

double safe_nonneg(double v) { return (std::isfinite(v) && v > 0.0) ? v : 0.0; }

double estimate_eta_days(const Simulation& sim, Id faction_id, Id start_system_id, Vec2 start_pos_mkm, double speed_km_s,
                         Id goal_system_id, Vec2 goal_pos_mkm, bool restrict_to_discovered) {
  if (start_system_id == kInvalidId || goal_system_id == kInvalidId) return std::numeric_limits<double>::infinity();
  if (speed_km_s <= 0.0) return std::numeric_limits<double>::infinity();
  auto plan = sim.plan_jump_route_from_pos(start_system_id, start_pos_mkm, faction_id, speed_km_s, goal_system_id,
                                          restrict_to_discovered, goal_pos_mkm);
  if (!plan) return std::numeric_limits<double>::infinity();
  return std::max(0.0, plan->total_eta_days);
}

}  // namespace

ColonistPlannerResult compute_colonist_plan(const Simulation& sim, Id faction_id, const ColonistPlannerOptions& opt) {
  ColonistPlannerResult out;

  const auto& st = sim.state();
  if (faction_id == kInvalidId || !find_ptr(st.factions, faction_id)) {
    out.ok = false;
    out.message = "Invalid faction.";
    return out;
  }

  const auto& cfg = sim.cfg();
  const double min_m = std::max(0.0, cfg.auto_colonist_min_transfer_millions);
  const double take_frac = std::clamp(cfg.auto_colonist_max_take_fraction_of_surplus, 0.0, 1.0);

  // Gather owned colonies and their positions (via Body).
  std::vector<Id> colony_ids;
  colony_ids.reserve(st.colonies.size());
  for (const auto& [cid, c] : st.colonies) {
    if (c.faction_id != faction_id) continue;
    colony_ids.push_back(cid);
  }
  std::sort(colony_ids.begin(), colony_ids.end());

  std::unordered_map<Id, Id> colony_system;
  std::unordered_map<Id, Vec2> colony_pos;
  colony_system.reserve(colony_ids.size() * 2 + 4);
  colony_pos.reserve(colony_ids.size() * 2 + 4);

  for (Id cid : colony_ids) {
    const Colony* c = find_ptr(st.colonies, cid);
    if (!c) continue;
    if (c->body_id == kInvalidId) continue;
    const Body* b = find_ptr(st.bodies, c->body_id);
    if (!b) continue;
    if (b->system_id == kInvalidId) continue;
    colony_system[cid] = b->system_id;
    colony_pos[cid] = b->position_mkm;
  }

  // Compute per-colony target/reserve/current/deficit/surplus.
  std::unordered_map<Id, ColonyPopInfo> pop;
  pop.reserve(colony_ids.size() * 2 + 4);

  for (Id cid : colony_ids) {
    const Colony* c = find_ptr(st.colonies, cid);
    if (!c) continue;

    ColonyPopInfo info;
    info.target = safe_nonneg(c->population_target_millions);
    info.reserve = safe_nonneg(c->population_reserve_millions);
    info.current = safe_nonneg(c->population_millions);
    info.floor = std::max(info.target, info.reserve);

    info.deficit = std::max(0.0, info.target - info.current);

    // By default, exporting population is opt-in: if a colony has no explicit
    // floor (target or reserve), do not treat it as a source. This prevents
    // accidental draining of colonies when only destinations are configured.
    const bool allow_export = (info.floor > kEps) || !cfg.auto_colonist_require_source_floor;
    if (allow_export) {
      info.surplus = std::max(0.0, info.current - info.floor);
    } else {
      info.surplus = 0.0;
    }

    if (info.deficit > kEps) info.reason = "Meet population target";

    pop[cid] = std::move(info);
  }

  // Collect deficit + surplus sets.
  std::vector<Id> deficit_colonies;
  std::vector<Id> surplus_colonies;
  deficit_colonies.reserve(colony_ids.size());
  surplus_colonies.reserve(colony_ids.size());

  for (Id cid : colony_ids) {
    const auto it = pop.find(cid);
    if (it == pop.end()) continue;
    const auto& info = it->second;
    if (info.deficit >= min_m + kEps) deficit_colonies.push_back(cid);
    if (info.surplus >= min_m + kEps) surplus_colonies.push_back(cid);
  }

  out.ok = true;

  if (deficit_colonies.empty()) {
    out.message = "No colonies need population.";
    return out;
  }

  // Candidate colonist transport ships.
  std::vector<Id> ship_ids;
  ship_ids.reserve(st.ships.size());
  for (const auto& [sid, sh] : st.ships) ship_ids.push_back(sid);
  std::sort(ship_ids.begin(), ship_ids.end());

  std::vector<Id> candidates;
  candidates.reserve(std::min<size_t>(ship_ids.size(), 256));

  const int max_ships = std::max(1, opt.max_ships);

  for (Id sid : ship_ids) {
    const Ship* sh = find_ptr(st.ships, sid);
    if (!sh) continue;
    if (sh->faction_id != faction_id) continue;

    if (opt.require_auto_colonist_transport_flag && !sh->auto_colonist_transport) continue;

    if (opt.exclude_fleet_ships && sim.fleet_for_ship(sid) != kInvalidId) continue;

    if (opt.require_idle) {
      const ShipOrders* so = find_ptr(st.ship_orders, sid);
      if (!ship_orders_is_idle_for_automation(so)) continue;
    }

    if (sh->system_id == kInvalidId) continue;
    if (sh->speed_km_s <= 0.0) continue;

    const ShipDesign* d = sim.find_design(sh->design_id);
    if (!d) continue;
    const double cap = std::max(0.0, d->colony_capacity_millions);
    if (cap < min_m + kEps) continue;

    candidates.push_back(sid);
    if (static_cast<int>(candidates.size()) >= max_ships) {
      if (sid != ship_ids.back()) {
        out.truncated = true;
        out.message = "Candidate ships truncated by max_ships.";
      }
      break;
    }
  }

  if (candidates.empty()) {
    if (out.message.empty()) out.message = "No eligible colonist transports.";
    return out;
  }

  // Planning state: remaining deficit/surplus amounts.
  std::unordered_map<Id, double> deficit_rem;
  std::unordered_map<Id, double> surplus_rem;
  deficit_rem.reserve(deficit_colonies.size() * 2 + 4);
  surplus_rem.reserve(surplus_colonies.size() * 2 + 4);

  for (Id cid : deficit_colonies) deficit_rem[cid] = std::max(0.0, pop[cid].deficit);
  for (Id cid : surplus_colonies) surplus_rem[cid] = std::max(0.0, pop[cid].surplus);

  out.assignments.reserve(candidates.size());

  // Greedy deterministic allocation:
  // 1) Ships already carrying colonists deliver to the best deficit destination.
  // 2) Empty ships pick up from a surplus colony then deliver to a deficit colony.
  for (Id sid : candidates) {
    const Ship* sh = find_ptr(st.ships, sid);
    if (!sh) continue;

    const ShipDesign* d = sim.find_design(sh->design_id);
    if (!d) continue;

    const double cap = std::max(0.0, d->colony_capacity_millions);
    const double embarked = safe_nonneg(sh->colonists_millions);

    // --- Phase 1: deliver embarked colonists ---
    if (embarked >= min_m + kEps) {
      Id best_dest = kInvalidId;
      double best_eta = std::numeric_limits<double>::infinity();

      for (Id dcid : deficit_colonies) {
        auto itrem = deficit_rem.find(dcid);
        if (itrem == deficit_rem.end() || itrem->second < min_m + kEps) continue;

        auto itsys = colony_system.find(dcid);
        auto itpos = colony_pos.find(dcid);
        if (itsys == colony_system.end() || itpos == colony_pos.end()) continue;

        const double eta =
            estimate_eta_days(sim, faction_id, sh->system_id, sh->position_mkm, sh->speed_km_s, itsys->second,
                              itpos->second, opt.restrict_to_discovered);

        if (!std::isfinite(eta)) continue;
        if (eta + 1e-9 < best_eta || (std::abs(eta - best_eta) <= 1e-9 && dcid < best_dest)) {
          best_eta = eta;
          best_dest = dcid;
        }
      }

      if (best_dest != kInvalidId && std::isfinite(best_eta)) {
        const double amt = std::min(embarked, deficit_rem[best_dest]);
        if (amt >= min_m + kEps) {
          ColonistAssignment asg;
          asg.kind = ColonistAssignmentKind::DeliverColonists;
          asg.ship_id = sid;
          asg.dest_colony_id = best_dest;
          asg.restrict_to_discovered = opt.restrict_to_discovered;
          asg.millions = amt;
          asg.eta_to_dest_days = best_eta;
          asg.eta_total_days = best_eta;
          asg.reason = pop[best_dest].reason;
          asg.note = "Deliver embarked colonists";

          out.assignments.push_back(std::move(asg));

          deficit_rem[best_dest] = std::max(0.0, deficit_rem[best_dest] - amt);
        }
      }

      continue;
    }

    // --- Phase 2: pickup + deliver ---
    if (cap < min_m + kEps) continue;
    if (take_frac <= kEps) continue;
    if (surplus_colonies.empty()) continue;

    Id best_src = kInvalidId;
    Id best_dest = kInvalidId;
    double best_eta_to_src = 0.0;
    double best_eta_to_dest = 0.0;
    double best_total = std::numeric_limits<double>::infinity();

    for (Id dcid : deficit_colonies) {
      auto itrem = deficit_rem.find(dcid);
      if (itrem == deficit_rem.end() || itrem->second < min_m + kEps) continue;

      auto dest_sys_it = colony_system.find(dcid);
      auto dest_pos_it = colony_pos.find(dcid);
      if (dest_sys_it == colony_system.end() || dest_pos_it == colony_pos.end()) continue;

      for (Id scid : surplus_colonies) {
        if (scid == dcid) continue;

        auto itsur = surplus_rem.find(scid);
        if (itsur == surplus_rem.end() || itsur->second < min_m + kEps) continue;

        auto src_sys_it = colony_system.find(scid);
        auto src_pos_it = colony_pos.find(scid);
        if (src_sys_it == colony_system.end() || src_pos_it == colony_pos.end()) continue;

        const double eta1 =
            estimate_eta_days(sim, faction_id, sh->system_id, sh->position_mkm, sh->speed_km_s, src_sys_it->second,
                              src_pos_it->second, opt.restrict_to_discovered);
        if (!std::isfinite(eta1)) continue;

        const double eta2 = estimate_eta_days(sim, faction_id, src_sys_it->second, src_pos_it->second, sh->speed_km_s,
                                              dest_sys_it->second, dest_pos_it->second, opt.restrict_to_discovered);
        if (!std::isfinite(eta2)) continue;

        const double total = eta1 + eta2;
        if (total + 1e-9 < best_total) {
          best_total = total;
          best_src = scid;
          best_dest = dcid;
          best_eta_to_src = eta1;
          best_eta_to_dest = eta2;
        } else if (std::abs(total - best_total) <= 1e-9) {
          // Deterministic tie-break: prefer lower dest id, then lower src id.
          if (dcid < best_dest || (dcid == best_dest && scid < best_src)) {
            best_src = scid;
            best_dest = dcid;
            best_eta_to_src = eta1;
            best_eta_to_dest = eta2;
          }
        }
      }
    }

    if (best_src == kInvalidId || best_dest == kInvalidId || !std::isfinite(best_total)) continue;

    const double available_take = std::max(0.0, surplus_rem[best_src] * take_frac);
    const double amt = std::min({deficit_rem[best_dest], cap, available_take});

    if (amt >= min_m + kEps) {
      ColonistAssignment asg;
      asg.kind = ColonistAssignmentKind::PickupAndDeliver;
      asg.ship_id = sid;
      asg.source_colony_id = best_src;
      asg.dest_colony_id = best_dest;
      asg.restrict_to_discovered = opt.restrict_to_discovered;
      asg.millions = amt;
      asg.eta_to_source_days = best_eta_to_src;
      asg.eta_to_dest_days = best_eta_to_dest;
      asg.eta_total_days = std::max(0.0, best_total);
      asg.reason = pop[best_dest].reason;
      asg.note = "Pickup + deliver";

      out.assignments.push_back(std::move(asg));

      deficit_rem[best_dest] = std::max(0.0, deficit_rem[best_dest] - amt);
      surplus_rem[best_src] = std::max(0.0, surplus_rem[best_src] - amt);
    }
  }

  if (out.assignments.empty() && out.message.empty()) {
    out.message = "No feasible population transfers.";
  } else if (out.message.empty()) {
    out.message = "OK.";
  }

  return out;
}

bool apply_colonist_assignment(Simulation& sim, const ColonistAssignment& asg, bool clear_existing_orders) {
  if (asg.ship_id == kInvalidId) return false;
  if (asg.dest_colony_id == kInvalidId) return false;
  if (asg.millions <= 0.0) return false;

  if (clear_existing_orders) sim.clear_orders(asg.ship_id);

  switch (asg.kind) {
    case ColonistAssignmentKind::DeliverColonists: {
      return sim.issue_unload_colonists(asg.ship_id, asg.dest_colony_id, asg.millions, asg.restrict_to_discovered);
    }
    case ColonistAssignmentKind::PickupAndDeliver: {
      if (asg.source_colony_id == kInvalidId) return false;
      bool ok = true;
      ok = ok && sim.issue_load_colonists(asg.ship_id, asg.source_colony_id, asg.millions, asg.restrict_to_discovered);
      ok = ok && sim.issue_unload_colonists(asg.ship_id, asg.dest_colony_id, asg.millions, asg.restrict_to_discovered);
      return ok;
    }
  }

  return false;
}

bool apply_colonist_plan(Simulation& sim, const ColonistPlannerResult& plan, bool clear_existing_orders) {
  if (!plan.ok) return false;
  bool ok = true;
  for (const auto& asg : plan.assignments) {
    ok = ok && apply_colonist_assignment(sim, asg, clear_existing_orders);
  }
  return ok;
}

}  // namespace nebula4x
