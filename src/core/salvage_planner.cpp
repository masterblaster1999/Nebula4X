#include "nebula4x/core/salvage_planner.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "nebula4x/core/simulation.h"

namespace nebula4x {

namespace {

constexpr double kEps = 1e-9;

double cargo_used_tons(const Ship& s) {
  double total = 0.0;
  for (const auto& [_, t] : s.cargo) total += std::max(0.0, t);
  return total;
}

double wreck_total_tons(const Wreck& w) {
  double total = 0.0;
  for (const auto& [_, t] : w.minerals) total += std::max(0.0, t);
  return total;
}

}  // namespace

SalvagePlannerResult compute_salvage_plan(const Simulation& sim, Id faction_id, const SalvagePlannerOptions& opt) {
  SalvagePlannerResult out;
  out.ok = false;

  const GameState& st = sim.state();
  if (faction_id == kInvalidId || st.factions.find(faction_id) == st.factions.end()) {
    out.message = "Invalid faction.";
    return out;
  }

  const double min_tons = (opt.min_tons > 0.0) ? std::max(1e-6, opt.min_tons)
                                               : std::max(1e-6, sim.cfg().auto_freight_min_transfer_tons);

  // --- Reserved wrecks (existing orders / caller-provided).
  //
  // We populate this *before* selecting/truncating candidate wrecks so that
  // reserved wrecks don't crowd out valid candidates when max_wrecks is small.
  std::unordered_set<Id> reserved_wrecks;
  reserved_wrecks.reserve(st.wrecks.size() * 2 + opt.reserved_wreck_ids.size() + 16);

  for (Id wid : opt.reserved_wreck_ids) {
    if (wid != kInvalidId) reserved_wrecks.insert(wid);
  }

  if (opt.reserve_wrecks_targeted_by_existing_orders) {
    auto mark_if_salvage = [&](const Order& o) {
      if (const auto* sw = std::get_if<SalvageWreck>(&o)) {
        if (sw->wreck_id != kInvalidId) reserved_wrecks.insert(sw->wreck_id);
      } else if (const auto* sl = std::get_if<SalvageWreckLoop>(&o)) {
        if (sl->wreck_id != kInvalidId) reserved_wrecks.insert(sl->wreck_id);
      }
    };

    for (const auto& [sid, so] : st.ship_orders) {
      const Ship* sh = find_ptr(st.ships, sid);
      if (!sh || sh->faction_id != faction_id) continue;

      for (const auto& o : so.queue) mark_if_salvage(o);

      if (so.repeat) {
        for (const auto& o : so.repeat_template) mark_if_salvage(o);
      }

      if (so.suspended) {
        for (const auto& o : so.suspended_queue) mark_if_salvage(o);
        if (so.suspended_repeat) {
          for (const auto& o : so.suspended_repeat_template) mark_if_salvage(o);
        }
      }
    }
  }

  // --- Friendly colonies (owned only; mirrors freight planner).
  std::vector<Id> colony_ids;
  colony_ids.reserve(st.colonies.size());
  for (const auto& [cid, c] : st.colonies) {
    if (c.faction_id == faction_id) colony_ids.push_back(cid);
  }
  std::sort(colony_ids.begin(), colony_ids.end());

  std::unordered_map<Id, Id> colony_system;
  std::unordered_map<Id, Vec2> colony_pos;
  colony_system.reserve(colony_ids.size() * 2 + 8);
  colony_pos.reserve(colony_ids.size() * 2 + 8);

  for (Id cid : colony_ids) {
    const Colony* c = find_ptr(st.colonies, cid);
    if (!c) continue;
    const Body* b = find_ptr(st.bodies, c->body_id);
    if (!b) continue;
    if (b->system_id == kInvalidId) continue;
    colony_system[cid] = b->system_id;
    colony_pos[cid] = b->position_mkm;
  }

  // Estimate travel-only ETA days via jump routing.
  auto estimate_eta_days_to_pos = [&](Id start_system_id, Vec2 start_pos_mkm, double speed_km_s, Id goal_system_id,
                                     Vec2 goal_pos_mkm) -> double {
    if (speed_km_s <= 0.0) return std::numeric_limits<double>::infinity();
    const auto plan = sim.plan_jump_route_from_pos(start_system_id, start_pos_mkm, faction_id, speed_km_s,
                                                   goal_system_id, opt.restrict_to_discovered, goal_pos_mkm);
    if (!plan) return std::numeric_limits<double>::infinity();
    return plan->total_eta_days;
  };

  // --- Candidate wrecks.
  struct WreckInfo {
    Id id{kInvalidId};
    Id system_id{kInvalidId};
    Vec2 pos{0.0, 0.0};
    double total_tons{0.0};
  };

  std::vector<WreckInfo> wrecks;
  wrecks.reserve(st.wrecks.size());
  for (const auto& [wid, w] : st.wrecks) {
    if (wid == kInvalidId) continue;
    if (w.system_id == kInvalidId) continue;
    if (!find_ptr(st.systems, w.system_id)) continue;

    if (reserved_wrecks.contains(wid)) continue;

    if (opt.restrict_to_discovered && !sim.is_system_discovered_by_faction(faction_id, w.system_id)) continue;
    if (opt.avoid_hostile_systems && !sim.detected_hostile_ships_in_system(faction_id, w.system_id).empty()) continue;

    const double total = wreck_total_tons(w);
    if (total < min_tons) continue;

    WreckInfo wi;
    wi.id = wid;
    wi.system_id = w.system_id;
    wi.pos = w.position_mkm;
    wi.total_tons = total;
    wrecks.push_back(wi);
  }

  std::sort(wrecks.begin(), wrecks.end(), [](const WreckInfo& a, const WreckInfo& b) {
    if (a.total_tons > b.total_tons + 1e-9) return true;
    if (b.total_tons > a.total_tons + 1e-9) return false;
    return a.id < b.id;
  });

  if (static_cast<int>(wrecks.size()) > std::max(1, opt.max_wrecks)) {
    wrecks.resize(std::max(1, opt.max_wrecks));
    out.truncated = true;
  }

  // --- Candidate ships.
  std::vector<Id> ship_ids;
  ship_ids.reserve(st.ships.size());
  for (const auto& [sid, _] : st.ships) ship_ids.push_back(sid);
  std::sort(ship_ids.begin(), ship_ids.end());

  struct ShipInfo {
    Id id{kInvalidId};
    Id system_id{kInvalidId};
    Vec2 pos{0.0, 0.0};
    double speed_km_s{0.0};
    double cargo_cap{0.0};
    double cargo_used{0.0};
    double cargo_free{0.0};
  };

  std::vector<ShipInfo> ships;
  ships.reserve(64);

  for (Id sid : ship_ids) {
    const Ship* sh = find_ptr(st.ships, sid);
    if (!sh) continue;
    if (sh->faction_id != faction_id) continue;

    if (opt.require_auto_salvage_flag && !sh->auto_salvage) continue;
    if (opt.exclude_conflicting_automation_flags) {
      if (sh->auto_mine) continue;
      if (sh->auto_freight) continue;
      if (sh->auto_explore) continue;
      if (sh->auto_colonize) continue;
      if (sh->auto_tanker) continue;
    }

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
    const double cap = std::max(0.0, d->cargo_tons);
    if (cap < min_tons) continue;

    ShipInfo si;
    si.id = sid;
    si.system_id = sh->system_id;
    si.pos = sh->position_mkm;
    si.speed_km_s = sh->speed_km_s;
    si.cargo_cap = cap;
    si.cargo_used = cargo_used_tons(*sh);
    si.cargo_free = std::max(0.0, cap - si.cargo_used);
    ships.push_back(si);

    if (static_cast<int>(ships.size()) >= std::max(1, opt.max_ships)) {
      if (sid != ship_ids.back()) out.truncated = true;
      break;
    }
  }

  // Sort ships by free capacity desc (then speed desc, then id) for greedy assignments.
  std::sort(ships.begin(), ships.end(), [](const ShipInfo& a, const ShipInfo& b) {
    if (a.cargo_free > b.cargo_free + 1e-9) return true;
    if (b.cargo_free > a.cargo_free + 1e-9) return false;
    if (a.speed_km_s > b.speed_km_s + 1e-9) return true;
    if (b.speed_km_s > a.speed_km_s + 1e-9) return false;
    return a.id < b.id;
  });

  // If no wrecks and no cargo to deliver, return early.
  if (ships.empty()) {
    out.ok = true;
    out.message = "No eligible ships.";
    return out;
  }

  // --- Assignments ---
  // (A) Deliver existing cargo first.
  std::vector<ShipInfo> salvage_ships;
  salvage_ships.reserve(ships.size());

  for (const auto& si : ships) {
    if (si.cargo_used >= min_tons && !colony_ids.empty()) {
      // Pick nearest friendly colony by ETA.
      Id best_col = kInvalidId;
      double best_eta = std::numeric_limits<double>::infinity();

      for (Id cid : colony_ids) {
        auto it_sys = colony_system.find(cid);
        auto it_pos = colony_pos.find(cid);
        if (it_sys == colony_system.end() || it_pos == colony_pos.end()) continue;

        const double eta = estimate_eta_days_to_pos(si.system_id, si.pos, si.speed_km_s, it_sys->second, it_pos->second);
        if (!std::isfinite(eta)) continue;
        if (best_col == kInvalidId || eta + 1e-9 < best_eta || (std::abs(eta - best_eta) <= 1e-9 && cid < best_col)) {
          best_col = cid;
          best_eta = eta;
        }
      }

      if (best_col != kInvalidId) {
        SalvageAssignment asg;
        asg.kind = SalvageAssignmentKind::DeliverCargo;
        asg.ship_id = si.id;
        asg.dest_colony_id = best_col;
        asg.restrict_to_discovered = opt.restrict_to_discovered;
        asg.avoid_hostile_systems = opt.avoid_hostile_systems;
        asg.eta_total_days = best_eta;
        asg.eta_to_dest_days = best_eta;
        asg.note = "Deliver existing cargo";
        out.assignments.push_back(std::move(asg));
        continue;
      }
    }

    salvage_ships.push_back(si);
  }

  // (B) Salvage one wreck per ship.
  if (!wrecks.empty()) {
    const double per_ton = std::max(0.0, sim.cfg().salvage_tons_per_day_per_cargo_ton);
    const double min_rate = std::max(0.0, sim.cfg().salvage_tons_per_day_min);

    for (const auto& si : salvage_ships) {
      if (si.cargo_free < min_tons) continue;

      Id best_wreck = kInvalidId;
      Id best_dest = kInvalidId;
      double best_score = -std::numeric_limits<double>::infinity();
      double best_eta_w = std::numeric_limits<double>::infinity();
      double best_eta_d = 0.0;
      double best_salv_days = 0.0;
      double best_expected = 0.0;
      double best_wreck_total = 0.0;

      for (const auto& w : wrecks) {
        if (reserved_wrecks.contains(w.id)) continue;

        const double eta_w = estimate_eta_days_to_pos(si.system_id, si.pos, si.speed_km_s, w.system_id, w.pos);
        if (!std::isfinite(eta_w)) continue;

        // Choose a destination colony (optional; if none exist, allow salvage-only assignments).
        Id dest = kInvalidId;
        double eta_d = 0.0;
        if (!colony_ids.empty()) {
          double best_eta_col = std::numeric_limits<double>::infinity();
          for (Id cid : colony_ids) {
            auto it_sys = colony_system.find(cid);
            auto it_pos = colony_pos.find(cid);
            if (it_sys == colony_system.end() || it_pos == colony_pos.end()) continue;

            const double eta = estimate_eta_days_to_pos(w.system_id, w.pos, si.speed_km_s, it_sys->second, it_pos->second);
            if (!std::isfinite(eta)) continue;
            if (dest == kInvalidId || eta + 1e-9 < best_eta_col ||
                (std::abs(eta - best_eta_col) <= 1e-9 && cid < dest)) {
              dest = cid;
              best_eta_col = eta;
            }
          }
          if (dest != kInvalidId) eta_d = best_eta_col;
        }

        const double expected = std::min(w.total_tons, si.cargo_free);
        if (expected < min_tons) continue;

        const double rate_per_day = std::max(min_rate, si.cargo_cap * per_ton);
        if (rate_per_day <= kEps) continue;

        const double salv_days = expected / rate_per_day;
        const double total = eta_w + salv_days + eta_d;
        const double score = expected / (1.0 + std::max(0.0, total));

        if (best_wreck == kInvalidId || score > best_score + 1e-9 ||
            (std::abs(score - best_score) <= 1e-9 &&
             (total + 1e-9 < (best_eta_w + best_salv_days + best_eta_d) ||
              (std::abs(total - (best_eta_w + best_salv_days + best_eta_d)) <= 1e-9 && w.id < best_wreck)))) {
          best_wreck = w.id;
          best_dest = dest;
          best_score = score;
          best_eta_w = eta_w;
          best_eta_d = eta_d;
          best_salv_days = salv_days;
          best_expected = expected;
          best_wreck_total = w.total_tons;
        }
      }

      if (best_wreck != kInvalidId) {
        reserved_wrecks.insert(best_wreck);

        SalvageAssignment asg;
        asg.kind = SalvageAssignmentKind::SalvageAndDeliver;
        asg.ship_id = si.id;
        asg.wreck_id = best_wreck;
        asg.dest_colony_id = best_dest;
        asg.restrict_to_discovered = opt.restrict_to_discovered;
        asg.avoid_hostile_systems = opt.avoid_hostile_systems;
        asg.mineral = "";
        asg.tons = 0.0;
        asg.eta_to_wreck_days = best_eta_w;
        asg.eta_to_dest_days = best_eta_d;
        asg.est_salvage_days = best_salv_days;
        asg.expected_salvage_tons = best_expected;
        asg.wreck_total_tons = best_wreck_total;
        asg.eta_total_days = best_eta_w + best_salv_days + best_eta_d;
        asg.note = (best_dest == kInvalidId) ? "Salvage (no drop-off colony)" : "Salvage + deliver";

        out.assignments.push_back(std::move(asg));
      }
    }
  }

  out.ok = true;
  if (out.assignments.empty()) {
    if (wrecks.empty()) {
      out.message = "No salvageable wrecks (or all wrecks filtered).";
    } else {
      out.message = "No viable assignments (ships may be busy/full or wrecks unreachable).";
    }
  } else {
    out.message = "OK";
  }

  return out;
}

bool apply_salvage_assignment(Simulation& sim, const SalvageAssignment& asg, bool clear_existing_orders) {
  if (asg.ship_id == kInvalidId) return false;

  bool ok = true;
  if (clear_existing_orders) {
    ok = ok && sim.clear_orders(asg.ship_id);
  }

  if (asg.kind == SalvageAssignmentKind::DeliverCargo) {
    if (asg.dest_colony_id == kInvalidId) return false;
    ok = ok && sim.issue_unload_mineral(asg.ship_id, asg.dest_colony_id, /*mineral=*/"", /*tons=*/0.0,
                                        asg.restrict_to_discovered);
  } else if (asg.kind == SalvageAssignmentKind::SalvageAndDeliver) {
    if (asg.wreck_id == kInvalidId) return false;
    ok = ok && sim.issue_salvage_wreck(asg.ship_id, asg.wreck_id, asg.mineral, asg.tons, asg.restrict_to_discovered);
    if (asg.dest_colony_id != kInvalidId) {
      ok = ok && sim.issue_unload_mineral(asg.ship_id, asg.dest_colony_id, /*mineral=*/"", /*tons=*/0.0,
                                          asg.restrict_to_discovered);
    }
  } else {
    return false;
  }

  if (!ok) {
    (void)sim.clear_orders(asg.ship_id);
  }

  return ok;
}

bool apply_salvage_plan(Simulation& sim, const SalvagePlannerResult& plan, bool clear_existing_orders) {
  if (!plan.ok) return false;
  bool ok = true;
  for (const auto& asg : plan.assignments) {
    ok = apply_salvage_assignment(sim, asg, clear_existing_orders) && ok;
  }
  return ok;
}

}  // namespace nebula4x
