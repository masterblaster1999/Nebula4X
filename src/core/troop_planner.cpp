#include "nebula4x/core/troop_planner.h"

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

struct ColonyStrengthInfo {
  double desired{0.0};
  double current{0.0};
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

TroopPlannerResult compute_troop_plan(const Simulation& sim, Id faction_id, const TroopPlannerOptions& opt) {
  TroopPlannerResult out;

  const auto& st = sim.state();
  if (faction_id == kInvalidId || !find_ptr(st.factions, faction_id)) {
    out.ok = false;
    out.message = "Invalid faction.";
    return out;
  }

  const auto& cfg = sim.cfg();
  const double min_strength = std::max(0.0, cfg.auto_troop_min_transfer_strength);
  const double take_frac = std::clamp(cfg.auto_troop_max_take_fraction_of_surplus, 0.0, 1.0);

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

  // Compute per-colony desired/current/deficit/surplus.
  std::unordered_map<Id, ColonyStrengthInfo> strength;
  strength.reserve(colony_ids.size() * 2 + 4);

  for (Id cid : colony_ids) {
    const Colony* c = find_ptr(st.colonies, cid);
    if (!c) continue;

    ColonyStrengthInfo info;
    info.desired = safe_nonneg(c->garrison_target_strength);

    // Battle-aware current strength (battle record is authoritative while active).
    const GroundBattle* b = nullptr;
    if (auto itb = st.ground_battles.find(cid); itb != st.ground_battles.end()) {
      b = &itb->second;
    }

    info.current = b ? safe_nonneg(b->defender_strength) : safe_nonneg(c->ground_forces);

    if (cfg.auto_troop_consider_active_battles && b && b->defender_faction_id == faction_id) {
      // Best-effort "don't lose" defender target based on square-law estimate.
      const double attacker = safe_nonneg(b->attacker_strength);
      const double forts = safe_nonneg(sim.fortification_points(*c));
      const double bonus = std::max(0.0, 1.0 + forts * cfg.fortification_defense_scale);
      const double factor = std::sqrt(std::max(0.0, bonus));
      const double margin = std::max(0.0, cfg.auto_troop_defense_margin_factor);

      double required_def = std::numeric_limits<double>::infinity();
      if (factor > 1e-9) required_def = attacker * margin / factor;
      required_def = safe_nonneg(required_def);

      if (required_def > info.desired + kEps) {
        info.desired = required_def;
        info.reason = "Reinforce defensive battle";
      }
    }

    if (info.reason.empty() && info.desired > kEps) {
      info.reason = "Meet garrison target";
    }

    info.deficit = std::max(0.0, info.desired - info.current);
    info.surplus = std::max(0.0, info.current - info.desired);

    strength[cid] = std::move(info);
  }

  // Collect deficit + surplus sets.
  std::vector<Id> deficit_colonies;
  std::vector<Id> surplus_colonies;
  deficit_colonies.reserve(colony_ids.size());
  surplus_colonies.reserve(colony_ids.size());

  for (Id cid : colony_ids) {
    const auto it = strength.find(cid);
    if (it == strength.end()) continue;
    const auto& info = it->second;
    if (info.deficit >= min_strength + kEps) deficit_colonies.push_back(cid);
    if (info.surplus >= min_strength + kEps) surplus_colonies.push_back(cid);
  }

  out.ok = true;

  if (deficit_colonies.empty()) {
    out.message = "No colonies need troops.";
    return out;
  }

  // Candidate troop transport ships.
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

    if (opt.require_auto_troop_transport_flag && !sh->auto_troop_transport) continue;

    if (opt.exclude_fleet_ships && sim.fleet_for_ship(sid) != kInvalidId) continue;

    if (opt.require_idle) {
      const ShipOrders* so = find_ptr(st.ship_orders, sid);
      const bool idle = (!so || so->queue.empty() || (so->repeat && so->repeat_count_remaining == 0));
      if (!idle) continue;
    }

    if (sh->system_id == kInvalidId) continue;
    if (sh->speed_km_s <= 0.0) continue;

    const ShipDesign* d = sim.find_design(sh->design_id);
    if (!d) continue;
    const double cap = std::max(0.0, d->troop_capacity);
    if (cap < min_strength + kEps) continue;

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
    if (out.message.empty()) out.message = "No eligible troop transports.";
    return out;
  }

  // Planning state: remaining deficit/surplus amounts.
  std::unordered_map<Id, double> deficit_rem;
  std::unordered_map<Id, double> surplus_rem;
  deficit_rem.reserve(deficit_colonies.size() * 2 + 4);
  surplus_rem.reserve(surplus_colonies.size() * 2 + 4);

  for (Id cid : deficit_colonies) deficit_rem[cid] = std::max(0.0, strength[cid].deficit);
  for (Id cid : surplus_colonies) surplus_rem[cid] = std::max(0.0, strength[cid].surplus);

  out.assignments.reserve(candidates.size());

  // Greedy deterministic allocation:
  // 1) Ships already carrying troops deliver to the best deficit destination.
  // 2) Empty ships pick up from a surplus colony then deliver to a deficit colony.
  for (Id sid : candidates) {
    const Ship* sh = find_ptr(st.ships, sid);
    if (!sh) continue;

    const ShipDesign* d = sim.find_design(sh->design_id);
    if (!d) continue;

    const double cap = std::max(0.0, d->troop_capacity);
    const double embarked = safe_nonneg(sh->troops);

    // --- Phase 1: deliver embarked troops ---
    if (embarked >= min_strength + kEps) {
      Id best_dest = kInvalidId;
      double best_eta = std::numeric_limits<double>::infinity();

      for (Id dcid : deficit_colonies) {
        auto itrem = deficit_rem.find(dcid);
        if (itrem == deficit_rem.end() || itrem->second < min_strength + kEps) continue;

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
        if (amt >= min_strength + kEps) {
          TroopAssignment asg;
          asg.kind = TroopAssignmentKind::DeliverTroops;
          asg.ship_id = sid;
          asg.dest_colony_id = best_dest;
          asg.restrict_to_discovered = opt.restrict_to_discovered;
          asg.strength = amt;
          asg.eta_to_dest_days = best_eta;
          asg.eta_total_days = best_eta;
          asg.reason = strength[best_dest].reason;
          asg.note = "Deliver embarked troops";

          out.assignments.push_back(std::move(asg));

          deficit_rem[best_dest] = std::max(0.0, deficit_rem[best_dest] - amt);
        }
      }

      continue;
    }

    // --- Phase 2: pickup + deliver ---
    if (cap < min_strength + kEps) continue;
    if (take_frac <= kEps) continue;
    if (surplus_colonies.empty()) continue;

    Id best_src = kInvalidId;
    Id best_dest = kInvalidId;
    double best_eta_to_src = 0.0;
    double best_eta_to_dest = 0.0;
    double best_total = std::numeric_limits<double>::infinity();

    for (Id dcid : deficit_colonies) {
      auto itrem = deficit_rem.find(dcid);
      if (itrem == deficit_rem.end() || itrem->second < min_strength + kEps) continue;

      auto dest_sys_it = colony_system.find(dcid);
      auto dest_pos_it = colony_pos.find(dcid);
      if (dest_sys_it == colony_system.end() || dest_pos_it == colony_pos.end()) continue;

      for (Id scid : surplus_colonies) {
        if (scid == dcid) continue;

        auto itsur = surplus_rem.find(scid);
        if (itsur == surplus_rem.end() || itsur->second < min_strength + kEps) continue;

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

    if (amt >= min_strength + kEps) {
      TroopAssignment asg;
      asg.kind = TroopAssignmentKind::PickupAndDeliver;
      asg.ship_id = sid;
      asg.source_colony_id = best_src;
      asg.dest_colony_id = best_dest;
      asg.restrict_to_discovered = opt.restrict_to_discovered;
      asg.strength = amt;
      asg.eta_to_source_days = best_eta_to_src;
      asg.eta_to_dest_days = best_eta_to_dest;
      asg.eta_total_days = std::max(0.0, best_total);
      asg.reason = strength[best_dest].reason;
      asg.note = "Pickup + deliver";

      out.assignments.push_back(std::move(asg));

      deficit_rem[best_dest] = std::max(0.0, deficit_rem[best_dest] - amt);
      surplus_rem[best_src] = std::max(0.0, surplus_rem[best_src] - amt);
    }
  }

  if (out.assignments.empty() && out.message.empty()) {
    // Deficits exist but we couldn't find any feasible transfers.
    out.message = "No feasible troop transfers.";
  } else if (out.message.empty()) {
    out.message = "OK.";
  }

  return out;
}

bool apply_troop_assignment(Simulation& sim, const TroopAssignment& asg, bool clear_existing_orders) {
  if (asg.ship_id == kInvalidId) return false;
  if (asg.dest_colony_id == kInvalidId) return false;
  if (asg.strength <= 0.0) return false;

  if (clear_existing_orders) sim.clear_orders(asg.ship_id);

  switch (asg.kind) {
    case TroopAssignmentKind::DeliverTroops: {
      return sim.issue_unload_troops(asg.ship_id, asg.dest_colony_id, asg.strength, asg.restrict_to_discovered);
    }
    case TroopAssignmentKind::PickupAndDeliver: {
      if (asg.source_colony_id == kInvalidId) return false;
      bool ok = true;
      ok = ok && sim.issue_load_troops(asg.ship_id, asg.source_colony_id, asg.strength, asg.restrict_to_discovered);
      ok = ok && sim.issue_unload_troops(asg.ship_id, asg.dest_colony_id, asg.strength, asg.restrict_to_discovered);
      return ok;
    }
  }

  return false;
}

bool apply_troop_plan(Simulation& sim, const TroopPlannerResult& plan, bool clear_existing_orders) {
  if (!plan.ok) return false;
  bool ok = true;
  for (const auto& asg : plan.assignments) {
    ok = ok && apply_troop_assignment(sim, asg, clear_existing_orders);
  }
  return ok;
}

}  // namespace nebula4x
