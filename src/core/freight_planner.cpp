#include "nebula4x/core/freight_planner.h"

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

// Convenience: safe lookup (0 if missing).
double map_get(const std::unordered_map<std::string, double>& m, const std::string& key) {
  auto it = m.find(key);
  return (it == m.end()) ? 0.0 : it->second;
}

// Deterministic label for LogisticsNeedKind.
//
// context_id is a best-effort string identifier that provides additional detail
// for certain need kinds (e.g. installation id for Construction/IndustryInput).
std::string need_kind_label(LogisticsNeedKind k, const std::string& context_id) {
  switch (k) {
    case LogisticsNeedKind::Shipyard:
      return "Shipyard";
    case LogisticsNeedKind::Construction:
      if (!context_id.empty()) return "Construction:" + context_id;
      return "Construction";
    case LogisticsNeedKind::IndustryInput:
      if (!context_id.empty()) return "IndustryInput:" + context_id;
      return "IndustryInput";
    case LogisticsNeedKind::StockpileTarget:
      if (!context_id.empty()) return "StockpileTarget:" + context_id;
      return "StockpileTarget";
    case LogisticsNeedKind::Fuel:
      return "Fuel";
  }
  return "Need";
}

}  // namespace

FreightPlannerResult compute_freight_plan(const Simulation& sim, Id faction_id,
                                         const FreightPlannerOptions& opt) {
  FreightPlannerResult out;
  out.ok = false;

  const GameState& st = sim.state();

  if (faction_id == kInvalidId || st.factions.find(faction_id) == st.factions.end()) {
    out.message = "Invalid faction.";
    return out;
  }

  const double min_tons = std::max(0.0, sim.cfg().auto_freight_min_transfer_tons);
  const double take_frac = std::clamp(sim.cfg().auto_freight_max_take_fraction_of_surplus, 0.0, 1.0);
  const bool bundle_multi = opt.bundle_multi_mineral.has_value() ? *opt.bundle_multi_mineral
                                                                 : sim.cfg().auto_freight_multi_mineral;

  // --- Collect friendly colony ids for this faction (owned colonies only, like logistics_needs_for_faction).
  std::vector<Id> colony_ids;
  colony_ids.reserve(st.colonies.size());
  for (const auto& [cid, c] : st.colonies) {
    if (c.faction_id == faction_id) colony_ids.push_back(cid);
  }
  std::sort(colony_ids.begin(), colony_ids.end());

  // Quick access: colony -> system/pos.
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

  // --- Build reserves/missing maps (mirrors tick_ai auto-freight).
  std::unordered_map<Id, std::unordered_map<std::string, double>> reserve_by_colony;
  std::unordered_map<Id, std::unordered_map<std::string, double>> missing_by_colony;

  // Optional reasons: colony->mineral->list.
  std::unordered_map<Id, std::unordered_map<std::string, std::vector<std::string>>> reasons_by_colony_mineral;

  reserve_by_colony.reserve(colony_ids.size() * 2 + 8);
  missing_by_colony.reserve(colony_ids.size() * 2 + 8);
  reasons_by_colony_mineral.reserve(colony_ids.size() * 2 + 8);

  for (Id cid : colony_ids) {
    const Colony* c = find_ptr(st.colonies, cid);
    if (!c) continue;
    reserve_by_colony[cid] = c->mineral_reserves;
  }

  const auto needs = sim.logistics_needs_for_faction(faction_id);
  for (const auto& n : needs) {
    if (n.colony_id == kInvalidId) continue;

    double& r = reserve_by_colony[n.colony_id][n.mineral];
    r = std::max(r, std::max(0.0, n.desired_tons));

    const double missing = std::max(0.0, n.missing_tons);
    if (missing > kEps) {
      double& m = missing_by_colony[n.colony_id][n.mineral];
      m = std::max(m, missing);

      const std::string reason = need_kind_label(n.kind, n.context_id);
      auto& list = reasons_by_colony_mineral[n.colony_id][n.mineral];
      if (std::find(list.begin(), list.end(), reason) == list.end()) {
        list.push_back(reason);
      }
    }
  }

  // A stable mineral ordering per colony based on missing tons.
  std::unordered_map<Id, std::vector<std::string>> need_minerals_by_colony;
  need_minerals_by_colony.reserve(missing_by_colony.size() * 2 + 8);

  std::vector<Id> dests_with_needs;
  for (const auto& [cid, mm] : missing_by_colony) {
    double total_missing = 0.0;
    for (const auto& [mineral, miss] : mm) total_missing += std::max(0.0, miss);
    if (total_missing < min_tons) continue;

    std::vector<std::pair<std::string, double>> tmp;
    tmp.reserve(mm.size());
    for (const auto& [mineral, miss] : mm) {
      if (miss >= min_tons) tmp.push_back({mineral, miss});
    }
    std::sort(tmp.begin(), tmp.end(), [](const auto& a, const auto& b) {
      if (a.second > b.second + 1e-9) return true;
      if (b.second > a.second + 1e-9) return false;
      return a.first < b.first;
    });

    std::vector<std::string> minerals;
    minerals.reserve(tmp.size());
    for (const auto& [mineral, _] : tmp) minerals.push_back(mineral);

    need_minerals_by_colony[cid] = std::move(minerals);
    dests_with_needs.push_back(cid);
  }
  std::sort(dests_with_needs.begin(), dests_with_needs.end());

  // Exportable minerals per colony.
  std::unordered_map<Id, std::unordered_map<std::string, double>> exportable_by_colony;
  exportable_by_colony.reserve(colony_ids.size() * 2 + 8);

  for (Id cid : colony_ids) {
    const Colony* c = find_ptr(st.colonies, cid);
    if (!c) continue;

    auto& exp = exportable_by_colony[cid];

    for (const auto& [mineral, have_raw] : c->minerals) {
      const double have = std::max(0.0, have_raw);
      const double reserve = [&]() {
        auto itc = reserve_by_colony.find(cid);
        if (itc == reserve_by_colony.end()) return 0.0;
        auto itm = itc->second.find(mineral);
        if (itm == itc->second.end()) return 0.0;
        return std::max(0.0, itm->second);
      }();

      const double exportable = std::max(0.0, have - reserve);
      if (exportable >= min_tons) exp[mineral] = exportable;
    }

    if (exp.empty()) exportable_by_colony.erase(cid);
  }

  // Utility: decrement missing/exportable maps (clamped).
  auto dec_missing = [&](Id cid, const std::string& mineral, double amount) {
    auto itc = missing_by_colony.find(cid);
    if (itc == missing_by_colony.end()) return;
    auto itm = itc->second.find(mineral);
    if (itm == itc->second.end()) return;
    itm->second = std::max(0.0, itm->second - std::max(0.0, amount));
  };

  auto dec_exportable = [&](Id cid, const std::string& mineral, double amount) {
    auto itc = exportable_by_colony.find(cid);
    if (itc == exportable_by_colony.end()) return;
    auto itm = itc->second.find(mineral);
    if (itm == itc->second.end()) return;
    itm->second = std::max(0.0, itm->second - std::max(0.0, amount));
    if (itm->second < min_tons) itc->second.erase(itm);
    if (itc->second.empty()) exportable_by_colony.erase(itc);
  };

  // Estimate travel-only ETA days via jump routing.
  auto estimate_eta_days_to_pos = [&](Id start_system_id, Vec2 start_pos_mkm, double speed_km_s, Id goal_system_id,
                                     Vec2 goal_pos_mkm) -> double {
    if (speed_km_s <= 0.0) return std::numeric_limits<double>::infinity();

    const auto plan = sim.plan_jump_route_from_pos(start_system_id, start_pos_mkm, faction_id, speed_km_s,
                                                   goal_system_id, opt.restrict_to_discovered, goal_pos_mkm);
    if (!plan) return std::numeric_limits<double>::infinity();
    return plan->total_eta_days;
  };

  // --- Candidate ships ---
  std::vector<Id> ship_ids;
  ship_ids.reserve(st.ships.size());
  for (const auto& [sid, _] : st.ships) ship_ids.push_back(sid);
  std::sort(ship_ids.begin(), ship_ids.end());

  std::vector<Id> candidate_ships;
  candidate_ships.reserve(64);

  for (Id sid : ship_ids) {
    const Ship* sh = find_ptr(st.ships, sid);
    if (!sh) continue;
    if (sh->faction_id != faction_id) continue;

    if (opt.require_auto_freight_flag && !sh->auto_freight) continue;

    // Avoid fighting the fleet movement logic.
    if (sim.fleet_for_ship(sid) != kInvalidId) continue;

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

    candidate_ships.push_back(sid);
    if (static_cast<int>(candidate_ships.size()) >= std::max(1, opt.max_ships)) {
      if (sid != ship_ids.back()) {
        out.truncated = true;
        out.message = "Candidate ships truncated by max_ships.";
      }
      break;
    }
  }

  // No work available.
  if (candidate_ships.empty()) {
    out.ok = true;
    if (out.message.empty()) out.message = "No eligible ships.";
    return out;
  }

  // --- Planning loop (mirrors the auto-freight assignment logic) ---
  out.assignments.reserve(candidate_ships.size());

  for (Id sid : candidate_ships) {
    const Ship* sh = find_ptr(st.ships, sid);
    if (!sh) continue;
    const ShipDesign* d = sim.find_design(sh->design_id);
    if (!d) continue;

    const double cap = std::max(0.0, d->cargo_tons);
    if (cap < min_tons) continue;

    // Current cargo.
    double used = 0.0;
    std::vector<std::string> cargo_minerals;
    cargo_minerals.reserve(sh->cargo.size());

    for (const auto& [mineral, tons_raw] : sh->cargo) {
      const double tons = std::max(0.0, tons_raw);
      if (tons <= kEps) continue;
      used += tons;
      cargo_minerals.push_back(mineral);
    }
    std::sort(cargo_minerals.begin(), cargo_minerals.end());

    const double free = std::max(0.0, cap - used);

    bool assigned = false;

    // 1) If ship already has cargo, deliver it to the best destination that needs it.
    if (used >= min_tons && !dests_with_needs.empty()) {
      struct Choice {
        Id dest{kInvalidId};
        double eff{std::numeric_limits<double>::infinity()};
        double eta{std::numeric_limits<double>::infinity()};
        double total{0.0};
        std::vector<FreightPlanItem> items;
      } best;

      for (Id dest_cid : dests_with_needs) {
        auto it_need = missing_by_colony.find(dest_cid);
        if (it_need == missing_by_colony.end()) continue;
        auto it_dest_sys = colony_system.find(dest_cid);
        auto it_dest_pos = colony_pos.find(dest_cid);
        if (it_dest_sys == colony_system.end() || it_dest_pos == colony_pos.end()) continue;

        std::vector<FreightPlanItem> items;
        items.reserve(bundle_multi ? cargo_minerals.size() : 1);
        double total = 0.0;

        for (const std::string& mineral : cargo_minerals) {
          const double have = map_get(sh->cargo, mineral);
          if (have < min_tons) continue;

          auto itm = it_need->second.find(mineral);
          if (itm == it_need->second.end()) continue;
          const double miss = std::max(0.0, itm->second);
          if (miss < min_tons) continue;

          const double amount = std::min(have, miss);
          if (amount < min_tons) continue;

          FreightPlanItem pi;
          pi.mineral = mineral;
          pi.tons = amount;
          // Reason (best-effort).
          auto itr = reasons_by_colony_mineral.find(dest_cid);
          if (itr != reasons_by_colony_mineral.end()) {
            auto itm2 = itr->second.find(mineral);
            if (itm2 != itr->second.end() && !itm2->second.empty()) {
              pi.reason = itm2->second.front();
            }
          }

          items.push_back(std::move(pi));
          total += amount;

          if (!bundle_multi) break;
        }

        if (total < min_tons) continue;

        const double eta = estimate_eta_days_to_pos(sh->system_id, sh->position_mkm, sh->speed_km_s,
                                                    it_dest_sys->second, it_dest_pos->second);
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
        FreightAssignment asg;
        asg.kind = FreightAssignmentKind::DeliverCargo;
        asg.ship_id = sid;
        asg.source_colony_id = kInvalidId;
        asg.dest_colony_id = best.dest;
        asg.restrict_to_discovered = opt.restrict_to_discovered;
        asg.items = std::move(best.items);
        asg.eta_to_source_days = 0.0;
        asg.eta_to_dest_days = best.eta;
        asg.eta_total_days = best.eta;
        asg.note = "Deliver existing cargo";

        // Consume missing for subsequent ships.
        for (const auto& it : asg.items) {
          dec_missing(asg.dest_colony_id, it.mineral, it.tons);
        }

        out.assignments.push_back(std::move(asg));
        assigned = true;
      }
    }

    if (assigned) continue;

    // 2) Otherwise pick a source+dest to satisfy missing minerals.
    if (free < min_tons) continue;
    if (dests_with_needs.empty()) continue;
    if (exportable_by_colony.empty()) continue;

    // Candidate sources (sorted by colony id for determinism).
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
      double eta1{0.0};
      double eta2{0.0};
      std::vector<FreightPlanItem> items;
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

        std::vector<FreightPlanItem> items;
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

          FreightPlanItem pi;
          pi.mineral = mineral;
          pi.tons = amount;
          // Reason (best-effort).
          auto itr = reasons_by_colony_mineral.find(dest_cid);
          if (itr != reasons_by_colony_mineral.end()) {
            auto itm2 = itr->second.find(mineral);
            if (itm2 != itr->second.end() && !itm2->second.empty()) {
              pi.reason = itm2->second.front();
            }
          }

          items.push_back(std::move(pi));
          total += amount;
          remaining -= amount;

          if (!bundle_multi) break;
        }

        if (total < min_tons) continue;

        const double eta1 = estimate_eta_days_to_pos(sh->system_id, sh->position_mkm, sh->speed_km_s,
                                                     it_src_sys->second, it_src_pos->second);
        if (!std::isfinite(eta1)) continue;
        const double eta2 = estimate_eta_days_to_pos(it_src_sys->second, it_src_pos->second, sh->speed_km_s,
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
          best.eta1 = eta1;
          best.eta2 = eta2;
          best.items = std::move(items);
        }
      }
    }

    if (best.source != kInvalidId && best.dest != kInvalidId && !best.items.empty()) {
      FreightAssignment asg;
      asg.kind = FreightAssignmentKind::PickupAndDeliver;
      asg.ship_id = sid;
      asg.source_colony_id = best.source;
      asg.dest_colony_id = best.dest;
      asg.restrict_to_discovered = opt.restrict_to_discovered;
      asg.items = std::move(best.items);
      asg.eta_to_source_days = best.eta1;
      asg.eta_to_dest_days = best.eta2;
      asg.eta_total_days = best.eta_total;
      asg.note = "Pickup + deliver";

      for (const auto& it : asg.items) {
        dec_exportable(asg.source_colony_id, it.mineral, it.tons);
        dec_missing(asg.dest_colony_id, it.mineral, it.tons);
      }

      out.assignments.push_back(std::move(asg));
      continue;
    }
  }

  out.ok = true;
  if (out.message.empty()) out.message = "OK";
  return out;
}

bool apply_freight_assignment(Simulation& sim, const FreightAssignment& asg, bool clear_existing_orders) {
  if (asg.ship_id == kInvalidId || asg.dest_colony_id == kInvalidId) return false;
  if (asg.items.empty()) return false;

  bool ok = true;

  if (clear_existing_orders) {
    ok = ok && sim.clear_orders(asg.ship_id);
  }

  if (asg.kind == FreightAssignmentKind::PickupAndDeliver && asg.source_colony_id != kInvalidId) {
    for (const auto& it : asg.items) {
      ok = ok && sim.issue_load_mineral(asg.ship_id, asg.source_colony_id, it.mineral, it.tons,
                                        asg.restrict_to_discovered);
    }
  }

  for (const auto& it : asg.items) {
    ok = ok && sim.issue_unload_mineral(asg.ship_id, asg.dest_colony_id, it.mineral, it.tons,
                                        asg.restrict_to_discovered);
  }

  if (!ok) {
    (void)sim.clear_orders(asg.ship_id);
  }

  return ok;
}

bool apply_freight_plan(Simulation& sim, const FreightPlannerResult& plan, bool clear_existing_orders) {
  if (!plan.ok) return false;

  bool ok = true;
  for (const auto& asg : plan.assignments) {
    ok = apply_freight_assignment(sim, asg, clear_existing_orders) && ok;
  }
  return ok;
}

}  // namespace nebula4x
