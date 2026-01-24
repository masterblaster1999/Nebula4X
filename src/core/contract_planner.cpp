#include "nebula4x/core/contract_planner.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "nebula4x/core/simulation.h"

namespace nebula4x {

namespace {

constexpr double kEps = 1e-9;

bool is_ship_idle(const GameState& st, Id ship_id) {
  const auto it = st.ship_orders.find(ship_id);
  if (it == st.ship_orders.end()) return true;
  const ShipOrders& so = it->second;
  if (so.suspended) return false;
  if (!so.queue.empty()) return false;
  if (so.repeat && !so.repeat_template.empty() && so.repeat_count_remaining != 0) return false;
  return true;
}

bool contract_target_pos(const GameState& st, const Contract& c, Id* out_sys, Vec2* out_pos) {
  if (out_sys) *out_sys = kInvalidId;
  if (out_pos) *out_pos = Vec2{0.0, 0.0};
  if (c.target_id == kInvalidId) return false;

  switch (c.kind) {
    case ContractKind::InvestigateAnomaly: {
      const auto* a = find_ptr(st.anomalies, c.target_id);
      if (!a) return false;
      if (out_sys) *out_sys = a->system_id;
      if (out_pos) *out_pos = a->position_mkm;
      return a->system_id != kInvalidId;
    }
    case ContractKind::SalvageWreck: {
      const auto* w = find_ptr(st.wrecks, c.target_id);
      if (!w) return false;
      if (out_sys) *out_sys = w->system_id;
      if (out_pos) *out_pos = w->position_mkm;
      return w->system_id != kInvalidId;
    }
    case ContractKind::SurveyJumpPoint: {
      const auto* jp = find_ptr(st.jump_points, c.target_id);
      if (!jp) return false;
      if (out_sys) *out_sys = jp->system_id;
      if (out_pos) *out_pos = jp->position_mkm;
      return jp->system_id != kInvalidId;
    }
    case ContractKind::EscortConvoy: {
      const auto* sh = find_ptr(st.ships, c.target_id);
      if (!sh) return false;
      if (out_sys) *out_sys = sh->system_id;
      if (out_pos) *out_pos = sh->position_mkm;
      return sh->system_id != kInvalidId;
    }
  }
  return false;
}

double clamp01(double v) {
  if (!std::isfinite(v)) return 0.0;
  if (v < 0.0) return 0.0;
  if (v > 1.0) return 1.0;
  return v;
}

double role_bonus_for_kind(ShipRole role, ContractKind kind) {
  // Small nudges so sensible ships win tie-breaks without overriding ETA too much.
  switch (kind) {
    case ContractKind::InvestigateAnomaly:
      if (role == ShipRole::Surveyor) return 0.20;
      if (role == ShipRole::Combatant) return 0.08;
      return 0.0;
    case ContractKind::SalvageWreck:
      if (role == ShipRole::Freighter) return 0.25;
      return 0.0;
    case ContractKind::SurveyJumpPoint:
      if (role == ShipRole::Surveyor) return 0.25;
      if (role == ShipRole::Combatant) return 0.05;
      return 0.0;
    case ContractKind::EscortConvoy:
      if (role == ShipRole::Combatant) return 0.25;
      if (role == ShipRole::Surveyor) return 0.05;
      return 0.0;
  }
  return 0.0;
}

double estimate_work_days_for_contract(const Simulation& sim, const Contract& c, Id faction_id, Id ship_id) {
  const GameState& st = sim.state();
  switch (c.kind) {
    case ContractKind::InvestigateAnomaly: {
      const auto* a = find_ptr(st.anomalies, c.target_id);
      if (!a) return 0.0;
      return std::max(0, a->investigation_days);
    }
    case ContractKind::SalvageWreck: {
      // Crude salvage-time estimate: expected tons / max(rate, eps).
      // This ignores travel back to a colony and any unloading overhead.
      const auto* w = find_ptr(st.wrecks, c.target_id);
      const auto* sh = find_ptr(st.ships, ship_id);
      if (!w || !sh) return 0.0;
      const auto* d = sim.find_design(sh->design_id);
      if (!d) return 0.0;

      double wreck_total = 0.0;
      for (const auto& [_, t] : w->minerals) wreck_total += std::max(0.0, t);

      double cargo_used = 0.0;
      for (const auto& [_, t] : sh->cargo) cargo_used += std::max(0.0, t);

      const double cap = std::max(0.0, d->cargo_tons);
      const double free = std::max(0.0, cap - cargo_used);
      const double expected = std::max(0.0, std::min(free, wreck_total));

      const double per_ton = std::max(0.0, sim.cfg().salvage_tons_per_day_per_cargo_ton);
      const double min_rate = std::max(0.0, sim.cfg().salvage_tons_per_day_min);
      const double rate = std::max(min_rate, per_ton * cap);
      if (rate <= kEps) return 0.0;
      return expected / rate;
    }
    case ContractKind::SurveyJumpPoint: {
      // Survey speed is tied to sensors and is handled in tick_ships.
      // We don't have a cheap closed form here, so assume "about 1 day".
      // The scoring primary driver is travel time anyway.
      (void)faction_id;
      return 1.0;
    }
    case ContractKind::EscortConvoy: {
      const auto* tgt = find_ptr(st.ships, c.target_id);
      if (!tgt) return 0.0;
      const Id dest_sys = c.target_id2;
      if (dest_sys == kInvalidId || st.systems.find(dest_sys) == st.systems.end()) return 0.0;
      if (tgt->system_id == dest_sys) return 0.0;
      const double sp = std::max(0.0, tgt->speed_km_s);
      if (sp <= kEps) return 0.0;
      const auto plan = sim.plan_jump_route_from_pos(tgt->system_id, tgt->position_mkm, tgt->faction_id, sp, dest_sys,
                                                    /*restrict_to_discovered=*/false);
      if (!plan) return 0.0;
      return std::max(0.0, plan->total_eta_days);
    }
  }
  return 0.0;
}

}  // namespace

ContractPlannerResult compute_contract_plan(const Simulation& sim, Id faction_id, const ContractPlannerOptions& opt) {
  ContractPlannerResult out;
  out.ok = false;

  const GameState& st = sim.state();
  if (faction_id == kInvalidId || st.factions.find(faction_id) == st.factions.end()) {
    out.message = "Invalid faction.";
    return out;
  }

  const int max_ships = std::clamp(opt.max_ships, 1, 8192);
  const int max_contracts = std::clamp(opt.max_contracts, 1, 8192);

  // --- Candidate contracts.
  struct ContractInfo {
    Id id{kInvalidId};
    ContractKind kind{ContractKind::InvestigateAnomaly};
    ContractStatus status{ContractStatus::Offered};
    Id system_id{kInvalidId};
    Vec2 pos{0.0, 0.0};
    double reward_rp{0.0};
    double risk{0.0};
    int hops{0};
  };

  std::vector<ContractInfo> contracts;
  contracts.reserve(64);

  std::vector<Id> contract_ids;
  contract_ids.reserve(st.contracts.size());
  for (const auto& [cid, _] : st.contracts) contract_ids.push_back(cid);
  std::sort(contract_ids.begin(), contract_ids.end());

  for (Id cid : contract_ids) {
    const auto* c = find_ptr(st.contracts, cid);
    if (!c) continue;
    if (c->assignee_faction_id != faction_id) continue;
    if (c->status == ContractStatus::Completed || c->status == ContractStatus::Expired || c->status == ContractStatus::Failed) continue;

    if (c->status == ContractStatus::Offered && !opt.include_offered) continue;
    if (c->status == ContractStatus::Accepted) {
      if (c->assigned_ship_id != kInvalidId && !opt.include_already_assigned) continue;
      if (c->assigned_ship_id == kInvalidId && !opt.include_accepted_unassigned) continue;
    }

    Id sys = kInvalidId;
    Vec2 pos{0.0, 0.0};
    if (!contract_target_pos(st, *c, &sys, &pos)) continue;
    if (sys == kInvalidId) continue;
    if (opt.restrict_to_discovered && !sim.is_system_discovered_by_faction(faction_id, sys)) continue;
    if (opt.avoid_hostile_systems && !sim.detected_hostile_ships_in_system(faction_id, sys).empty()) continue;

    // Filter out already-resolved anomaly offers (stale contracts can exist briefly between ticks).
    if (c->kind == ContractKind::InvestigateAnomaly) {
      if (const auto* a = find_ptr(st.anomalies, c->target_id)) {
        if (a->resolved) {
          // If it's already resolved by us, there's nothing to do.
          // If resolved by someone else, it's impossible.
          continue;
        }
      }
    }

    // Filter out wreck offers that have no minerals.
    if (c->kind == ContractKind::SalvageWreck) {
      const auto* w = find_ptr(st.wrecks, c->target_id);
      if (!w) continue;
      double total = 0.0;
      for (const auto& [_, t] : w->minerals) total += std::max(0.0, t);
      if (total <= kEps) continue;
    }

    // Filter out stale/unrunnable escort offers.
    if (c->kind == ContractKind::EscortConvoy) {
      const auto* convoy = find_ptr(st.ships, c->target_id);
      if (!convoy) continue;
      if (c->target_id2 == kInvalidId || st.systems.find(c->target_id2) == st.systems.end()) continue;
      if (convoy->system_id == c->target_id2) continue;

      // For escort, "avoid hostiles" should apply to the destination too.
      if (opt.avoid_hostile_systems && !sim.detected_hostile_ships_in_system(faction_id, c->target_id2).empty()) {
        continue;
      }
    }

    ContractInfo ci;
    ci.id = cid;
    ci.kind = c->kind;
    ci.status = c->status;
    ci.system_id = sys;
    ci.pos = pos;
    ci.reward_rp = std::max(0.0, c->reward_research_points);
    ci.risk = clamp01(c->risk_estimate);
    ci.hops = std::max(0, c->hops_estimate);
    contracts.push_back(ci);

    if ((int)contracts.size() >= max_contracts) {
      if (cid != contract_ids.back()) out.truncated = true;
      break;
    }
  }

  if (contracts.empty()) {
    out.ok = true;
    out.message = "No eligible contracts.";
    return out;
  }

  // --- Candidate ships.
  struct ShipInfo {
    Id id{kInvalidId};
    Id system_id{kInvalidId};
    Vec2 pos{0.0, 0.0};
    double speed_km_s{0.0};
    ShipRole role{ShipRole::Unknown};
    double cargo_cap_tons{0.0};
    double cargo_used_tons{0.0};
    double sensor_range_mkm{0.0};
  };

  std::vector<Id> ship_ids;
  ship_ids.reserve(st.ships.size());
  for (const auto& [sid, _] : st.ships) ship_ids.push_back(sid);
  std::sort(ship_ids.begin(), ship_ids.end());

  std::vector<ShipInfo> ships;
  ships.reserve(64);

  for (Id sid : ship_ids) {
    const auto* sh = find_ptr(st.ships, sid);
    if (!sh) continue;
    if (sh->faction_id != faction_id) continue;
    if (sh->system_id == kInvalidId) continue;
    if (sh->speed_km_s <= 0.0) continue;
    if (opt.exclude_fleet_ships && sim.fleet_for_ship(sid) != kInvalidId) continue;
    if (opt.require_idle && !is_ship_idle(st, sid)) continue;

    const ShipDesign* d = sim.find_design(sh->design_id);
    if (!d) continue;

    double cargo_used = 0.0;
    for (const auto& [_, t] : sh->cargo) cargo_used += std::max(0.0, t);

    ShipInfo si;
    si.id = sid;
    si.system_id = sh->system_id;
    si.pos = sh->position_mkm;
    si.speed_km_s = sh->speed_km_s;
    si.role = d->role;
    si.cargo_cap_tons = std::max(0.0, d->cargo_tons);
    si.cargo_used_tons = cargo_used;
    si.sensor_range_mkm = std::max(0.0, d->sensor_range_mkm);
    ships.push_back(si);

    if ((int)ships.size() >= max_ships) {
      if (sid != ship_ids.back()) out.truncated = true;
      break;
    }
  }

  if (ships.empty()) {
    out.ok = true;
    out.message = "No eligible ships.";
    return out;
  }

  // --- Edges (contract, ship) scored.
  struct Edge {
    Id contract_id{kInvalidId};
    Id ship_id{kInvalidId};
    double eta_days{0.0};
    double work_days{0.0};
    double score{0.0};
  };

  std::vector<Edge> edges;
  edges.reserve(contracts.size() * std::min<std::size_t>(ships.size(), 128));

  const double min_tons = std::max(1e-6, sim.cfg().auto_freight_min_transfer_tons);
  const double risk_penalty = std::max(0.0, opt.risk_penalty);
  const double hop_overhead = std::max(0.0, opt.hop_overhead_days);
  const double escort_jump_eta_slack_days = 0.5;
  const double escort_speed_eps = 1e-9;

  for (const auto& c : contracts) {
    const auto* full_c = find_ptr(st.contracts, c.id);
    if (!full_c) continue;

    // Precompute escort-leg work for EscortConvoy contracts (same for all ships).
    double escort_leg_days = 0.0;
    int escort_leg_hops = 0;
    double escort_convoy_speed_km_s = 0.0;
    if (c.kind == ContractKind::EscortConvoy) {
      const auto* convoy = find_ptr(st.ships, full_c->target_id);
      if (!convoy) continue;
      const Id dest_sys = full_c->target_id2;
      if (dest_sys == kInvalidId || st.systems.find(dest_sys) == st.systems.end()) continue;
      if (convoy->system_id == dest_sys) continue;
      escort_convoy_speed_km_s = std::max(0.0, convoy->speed_km_s);
      const auto convoy_plan = sim.plan_jump_route_from_pos(convoy->system_id, convoy->position_mkm, convoy->faction_id,
                                                           convoy->speed_km_s, dest_sys,
                                                           /*restrict_to_discovered=*/false);
      if (!convoy_plan || !std::isfinite(convoy_plan->total_eta_days)) continue;
      escort_leg_days = std::max(0.0, convoy_plan->total_eta_days);
      escort_leg_hops = (int)convoy_plan->jump_ids.size();
    }

    for (const auto& sh : ships) {
      // Basic capability filters per contract.
      if (c.kind == ContractKind::SalvageWreck) {
        if (sh.cargo_cap_tons < min_tons) continue;
        // Prefer empty-ish ships; still allow some cargo.
        if (sh.cargo_cap_tons - sh.cargo_used_tons < min_tons) continue;
      }
      if (c.kind == ContractKind::SurveyJumpPoint) {
        // Surveying requires online sensors; ships with no sensors can't progress.
        if (sh.sensor_range_mkm <= kEps) continue;
      }

      if (c.kind == ContractKind::EscortConvoy) {
        // If you're slower than the convoy, you're very unlikely to remain within escort range.
        if (escort_convoy_speed_km_s > escort_speed_eps && sh.speed_km_s + 1e-9 < escort_convoy_speed_km_s) continue;
      }

      // Travel ETA.
      const auto plan = sim.plan_jump_route_from_pos(sh.system_id, sh.pos, faction_id, sh.speed_km_s, c.system_id,
                                                     opt.restrict_to_discovered, c.pos);
      if (!plan) continue;
      const double eta = plan->total_eta_days;
      if (!std::isfinite(eta)) continue;

      // Escort contracts are time-sensitive: if we can't even reach the convoy's
      // current system before it arrives at its destination, this assignment is
      // almost certainly doomed.
      if (c.kind == ContractKind::EscortConvoy) {
        const double eta_to_system = plan->eta_days;
        if (!std::isfinite(eta_to_system)) continue;
        if (eta_to_system > escort_leg_days + escort_jump_eta_slack_days) continue;
      }

      const double work = (c.kind == ContractKind::EscortConvoy)
                            ? escort_leg_days
                            : estimate_work_days_for_contract(sim, *full_c, faction_id, sh.id);

      int hops = std::max(0, c.hops);
      if (c.kind == ContractKind::EscortConvoy) {
        // Use a more accurate hop estimate: ship->convoy hops + convoy remaining hops.
        hops = (int)plan->jump_ids.size() + escort_leg_hops;
      }

      const double total = std::max(0.0, eta) + std::max(0.0, work) + hop_overhead * (double)hops;

      // Heuristic: RP per (day + 1), adjusted for risk and role.
      double score = c.reward_rp / (total + 1.0);
      if (risk_penalty > 0.0) {
        score *= std::max(0.0, 1.0 - clamp01(c.risk) * risk_penalty);
      }
      score *= (1.0 + role_bonus_for_kind(sh.role, c.kind));

      Edge e;
      e.contract_id = c.id;
      e.ship_id = sh.id;
      e.eta_days = eta;
      e.work_days = work;
      e.score = score;
      edges.push_back(e);
    }
  }

  if (edges.empty()) {
    out.ok = true;
    out.message = "No feasible ship/contract pairs (no route or capability mismatch).";
    return out;
  }

  // Sort edges by score (desc), then ETA (asc), then ids.
  std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) {
    if (a.score > b.score + 1e-12) return true;
    if (b.score > a.score + 1e-12) return false;
    if (a.eta_days + 1e-9 < b.eta_days) return true;
    if (b.eta_days + 1e-9 < a.eta_days) return false;
    if (a.contract_id != b.contract_id) return a.contract_id < b.contract_id;
    return a.ship_id < b.ship_id;
  });

  std::unordered_set<Id> used_ships;
  used_ships.reserve(ships.size() * 2 + 8);
  std::unordered_set<Id> used_contracts;
  used_contracts.reserve(contracts.size() * 2 + 8);

  for (const auto& e : edges) {
    if (used_ships.contains(e.ship_id)) continue;
    if (used_contracts.contains(e.contract_id)) continue;

    ContractAssignment asg;
    asg.contract_id = e.contract_id;
    asg.ship_id = e.ship_id;
    asg.restrict_to_discovered = opt.restrict_to_discovered;
    asg.clear_existing_orders = opt.clear_orders_before_apply;
    asg.eta_days = e.eta_days;
    asg.work_days = e.work_days;
    asg.score = e.score;
    out.assignments.push_back(std::move(asg));

    used_ships.insert(e.ship_id);
    used_contracts.insert(e.contract_id);

    if ((int)out.assignments.size() >= max_contracts) break;
  }

  out.ok = true;
  if (out.assignments.empty()) {
    out.message = "No assignments found.";
  } else {
    out.message = "Planned " + std::to_string((unsigned long long)out.assignments.size()) + " assignment(s).";
  }
  return out;
}

bool apply_contract_assignment(Simulation& sim, const ContractAssignment& asg,
                               bool push_event, std::string* error) {
  if (asg.contract_id == kInvalidId || asg.ship_id == kInvalidId) {
    if (error) *error = "Invalid assignment.";
    return false;
  }
  return sim.assign_contract_to_ship(asg.contract_id, asg.ship_id, asg.clear_existing_orders,
                                     asg.restrict_to_discovered, push_event, error);
}

bool apply_contract_plan(Simulation& sim, const ContractPlannerResult& plan,
                         bool push_event, std::string* error) {
  std::string err;
  for (const auto& asg : plan.assignments) {
    err.clear();
    if (!apply_contract_assignment(sim, asg, push_event, &err)) {
      if (error) {
        *error = err.empty() ? "Failed to apply contract plan." : err;
      }
      return false;
    }
  }
  return true;
}

}  // namespace nebula4x
