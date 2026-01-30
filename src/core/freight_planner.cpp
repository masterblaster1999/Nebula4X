#include "nebula4x/core/freight_planner.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <queue>
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
    case LogisticsNeedKind::TroopTraining:
      if (!context_id.empty()) return "TroopTraining:" + context_id;
      return "TroopTraining";
    case LogisticsNeedKind::IndustryInput:
      if (!context_id.empty()) return "IndustryInput:" + context_id;
      return "IndustryInput";
    case LogisticsNeedKind::StockpileTarget:
      if (!context_id.empty()) return "StockpileTarget:" + context_id;
      return "StockpileTarget";
    case LogisticsNeedKind::Fuel:
      return "Fuel";
    case LogisticsNeedKind::Rearm:
      return "Rearm";
    case LogisticsNeedKind::Maintenance:
      return "Maintenance";
  }
  return "Need";
}

// Priority queue ordering for freight candidates.
//
// We want the "best" candidate at the top:
//   1) lowest eta/tons (efficiency)
//   2) lowest eta_total
//   3) highest total_tons
//   4) lowest ship id (determinism)
//   5) lowest dest id
//   6) lowest source id
struct FreightCandidate {
  FreightAssignmentKind kind{FreightAssignmentKind::PickupAndDeliver};

  Id ship_id{kInvalidId};
  Id source{kInvalidId};
  Id dest{kInvalidId};

  bool restrict_to_discovered{true};

  // Minerals loaded at the source colony (may be empty).
  std::vector<FreightPlanItem> load_items;
  // Minerals unloaded at the destination colony (includes cargo already on the ship).
  std::vector<FreightPlanItem> unload_items;

  // How many tons of the unloaded cargo come from the ship's existing holds.
  double deliver_from_cargo_tons{0.0};

  double eta1{0.0};
  double eta2{0.0};
  double eta_total{0.0};

  double total_tons{0.0};
  double eff{std::numeric_limits<double>::infinity()};

  // Per-ship stamp used to invalidate stale heap entries.
  std::uint64_t stamp{0};
};

struct FreightCandidateWorse {
  bool operator()(const FreightCandidate& a, const FreightCandidate& b) const {
    auto gt = [](double x, double y) { return x > y + 1e-9; };
    auto lt = [](double x, double y) { return x < y - 1e-9; };

    if (gt(a.eff, b.eff)) return true;
    if (gt(b.eff, a.eff)) return false;

    if (gt(a.eta_total, b.eta_total)) return true;
    if (gt(b.eta_total, a.eta_total)) return false;

    if (lt(a.total_tons, b.total_tons)) return true;
    if (lt(b.total_tons, a.total_tons)) return false;

    if (a.ship_id != b.ship_id) return a.ship_id > b.ship_id;
    if (a.dest != b.dest) return a.dest > b.dest;
    if (a.source != b.source) return a.source > b.source;

    if (a.kind != b.kind) return static_cast<int>(a.kind) > static_cast<int>(b.kind);

    // stamp shouldn't matter, but keep strict weak ordering.
    return a.stamp > b.stamp;
  }
};

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

  const double min_tons = std::max(1e-6, sim.cfg().auto_freight_min_transfer_tons);
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

  auto first_reason = [&](Id dest_cid, const std::string& mineral) -> std::string {
    auto itr = reasons_by_colony_mineral.find(dest_cid);
    if (itr == reasons_by_colony_mineral.end()) return {};
    auto itm = itr->second.find(mineral);
    if (itm == itr->second.end() || itm->second.empty()) return {};
    return itm->second.front();
  };

  // --- Candidate ships ---
  struct ShipPlanData {
    Id ship_id{kInvalidId};
    const Ship* sh{nullptr};
    const ShipDesign* d{nullptr};

    double cap{0.0};
    double used{0.0};
    double free{0.0};

    std::vector<std::string> cargo_minerals;

    // Used to invalidate stale heap entries.
    std::uint64_t stamp{0};
  };

  std::vector<Id> ship_ids;
  ship_ids.reserve(st.ships.size());
  for (const auto& [sid, _] : st.ships) ship_ids.push_back(sid);
  std::sort(ship_ids.begin(), ship_ids.end());

  std::vector<ShipPlanData> ships;
  ships.reserve(64);

  std::unordered_map<Id, std::size_t> ship_index;
  ship_index.reserve(128);

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

    ShipPlanData sd;
    sd.ship_id = sid;
    sd.sh = sh;
    sd.d = d;
    sd.cap = cap;

    // Current cargo.
    sd.used = 0.0;
    sd.cargo_minerals.reserve(sh->cargo.size());
    for (const auto& [mineral, tons_raw] : sh->cargo) {
      const double tons = std::max(0.0, tons_raw);
      if (tons <= kEps) continue;
      sd.used += tons;
      sd.cargo_minerals.push_back(mineral);
    }
    std::sort(sd.cargo_minerals.begin(), sd.cargo_minerals.end());

    sd.free = std::max(0.0, cap - sd.used);

    ship_index[sid] = ships.size();
    ships.push_back(std::move(sd));

    if (static_cast<int>(ships.size()) >= std::max(1, opt.max_ships)) {
      if (sid != ship_ids.back()) {
        out.truncated = true;
        out.message = "Candidate ships truncated by max_ships.";
      }
      break;
    }
  }

  // No work available.
  if (ships.empty()) {
    out.ok = true;
    if (out.message.empty()) out.message = "No eligible ships.";
    return out;
  }

  // --- Candidate generation helpers ---
  auto make_delivery_candidate = [&](const ShipPlanData& sd, Id dest_cid) -> std::optional<FreightCandidate> {
    if (!sd.sh) return std::nullopt;
    if (sd.used < min_tons) return std::nullopt;

    auto it_need = missing_by_colony.find(dest_cid);
    if (it_need == missing_by_colony.end()) return std::nullopt;
    auto it_dest_sys = colony_system.find(dest_cid);
    auto it_dest_pos = colony_pos.find(dest_cid);
    if (it_dest_sys == colony_system.end() || it_dest_pos == colony_pos.end()) return std::nullopt;

    std::vector<FreightPlanItem> unload_items;
    unload_items.reserve(bundle_multi ? sd.cargo_minerals.size() : 1);

    double total = 0.0;

    for (const std::string& mineral : sd.cargo_minerals) {
      const double have = map_get(sd.sh->cargo, mineral);
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
      pi.reason = first_reason(dest_cid, mineral);

      unload_items.push_back(std::move(pi));
      total += amount;

      if (!bundle_multi) break;
    }

    if (total < min_tons) return std::nullopt;

    const double eta = estimate_eta_days_to_pos(sd.sh->system_id, sd.sh->position_mkm, sd.sh->speed_km_s,
                                                it_dest_sys->second, it_dest_pos->second);
    if (!std::isfinite(eta)) return std::nullopt;

    FreightCandidate c;
    c.kind = FreightAssignmentKind::DeliverCargo;
    c.ship_id = sd.ship_id;
    c.source = kInvalidId;
    c.dest = dest_cid;
    c.restrict_to_discovered = opt.restrict_to_discovered;
    c.load_items.clear();
    c.unload_items = std::move(unload_items);
    c.deliver_from_cargo_tons = total;
    c.eta1 = 0.0;
    c.eta2 = eta;
    c.eta_total = eta;
    c.total_tons = total;
    c.eff = eta / std::max(1e-9, total);
    return c;
  };

  auto make_pickup_candidate = [&](const ShipPlanData& sd, Id src_cid, Id dest_cid) -> std::optional<FreightCandidate> {
    if (!sd.sh) return std::nullopt;
    if (sd.free < min_tons) return std::nullopt;

    if (src_cid == dest_cid) return std::nullopt;

    auto it_need_list = need_minerals_by_colony.find(dest_cid);
    if (it_need_list == need_minerals_by_colony.end()) return std::nullopt;

    auto it_exp_c = exportable_by_colony.find(src_cid);
    if (it_exp_c == exportable_by_colony.end()) return std::nullopt;

    auto it_src_sys = colony_system.find(src_cid);
    auto it_src_pos = colony_pos.find(src_cid);
    auto it_dest_sys = colony_system.find(dest_cid);
    auto it_dest_pos = colony_pos.find(dest_cid);
    if (it_src_sys == colony_system.end() || it_src_pos == colony_pos.end()) return std::nullopt;
    if (it_dest_sys == colony_system.end() || it_dest_pos == colony_pos.end()) return std::nullopt;

    // We build separate load/unload item lists so we can support "top up then deliver"
    // plans for partially-loaded ships.
    std::vector<FreightPlanItem> load_items;
    std::vector<FreightPlanItem> unload_items;
    load_items.reserve(bundle_multi ? it_need_list->second.size() : 1);
    unload_items.reserve(bundle_multi ? it_need_list->second.size() : 1);

    double remaining = sd.free;
    double loaded_total = 0.0;
    double deliver_from_cargo_total = 0.0;

    const auto missing_for = [&](const std::string& mineral) -> double {
      auto itc = missing_by_colony.find(dest_cid);
      if (itc == missing_by_colony.end()) return 0.0;
      auto itm = itc->second.find(mineral);
      if (itm == itc->second.end()) return 0.0;
      return std::max(0.0, itm->second);
    };

    // When bundling is disabled, we intentionally pick a single mineral to move.
    std::string focus_mineral;
    if (!bundle_multi) {
      // Prefer delivering an on-board mineral that is missing at the destination.
      for (const std::string& mineral : it_need_list->second) {
        const double have = map_get(sd.sh->cargo, mineral);
        if (have < min_tons) continue;
        const double miss = missing_for(mineral);
        if (miss < min_tons) continue;
        focus_mineral = mineral;
        break;
      }
      // Otherwise, pick the first mineral the source can provide.
      if (focus_mineral.empty()) {
        for (const std::string& mineral : it_need_list->second) {
          if (missing_for(mineral) < min_tons) continue;
          auto it_exp = it_exp_c->second.find(mineral);
          if (it_exp == it_exp_c->second.end()) continue;
          if (std::max(0.0, it_exp->second) < min_tons) continue;
          focus_mineral = mineral;
          break;
        }
      }
      if (focus_mineral.empty()) return std::nullopt;
    }

    auto consider = [&](const std::string& mineral) {
      if (remaining < min_tons) return;

      const double miss = missing_for(mineral);
      if (miss < min_tons) return;

      // Portion that can be satisfied from cargo already on the ship.
      double deliver_from_cargo = 0.0;
      const double have = map_get(sd.sh->cargo, mineral);
      if (have >= min_tons) {
        deliver_from_cargo = std::min(have, miss);
        if (deliver_from_cargo < min_tons) deliver_from_cargo = 0.0;
      }

      const double miss_after_cargo = std::max(0.0, miss - deliver_from_cargo);

      // Portion to pick up from the source colony.
      double load_amount = 0.0;
      if (miss_after_cargo >= min_tons) {
        auto it_exp = it_exp_c->second.find(mineral);
        if (it_exp != it_exp_c->second.end()) {
          const double avail = std::max(0.0, it_exp->second);
          if (avail >= min_tons) {
            const double take_cap = avail * take_frac;
            load_amount = std::min({remaining, miss_after_cargo, take_cap});
            if (load_amount < min_tons) load_amount = 0.0;
          }
        }
      }

      if (deliver_from_cargo <= 0.0 && load_amount <= 0.0) return;

      if (load_amount > 0.0) {
        FreightPlanItem li;
        li.mineral = mineral;
        li.tons = load_amount;
        // li.reason intentionally empty (the reason is destination-side).
        load_items.push_back(std::move(li));
        loaded_total += load_amount;
        remaining -= load_amount;
      }

      const double unload_amount = deliver_from_cargo + load_amount;
      if (unload_amount >= min_tons) {
        FreightPlanItem ui;
        ui.mineral = mineral;
        ui.tons = unload_amount;
        ui.reason = first_reason(dest_cid, mineral);
        unload_items.push_back(std::move(ui));
        deliver_from_cargo_total += deliver_from_cargo;
      }
    };

    if (bundle_multi) {
      for (const std::string& mineral : it_need_list->second) {
        consider(mineral);
        if (remaining < min_tons) break;
      }
    } else {
      consider(focus_mineral);
    }

    // This is a pickup candidate: it must load something meaningful at the source.
    if (loaded_total < min_tons) return std::nullopt;

    const double total_unload = [&]() {
      double s = 0.0;
      for (const auto& it : unload_items) s += std::max(0.0, it.tons);
      return s;
    }();
    if (total_unload < min_tons) return std::nullopt;

    const double eta1 = estimate_eta_days_to_pos(sd.sh->system_id, sd.sh->position_mkm, sd.sh->speed_km_s,
                                                 it_src_sys->second, it_src_pos->second);
    if (!std::isfinite(eta1)) return std::nullopt;

    const double eta2 = estimate_eta_days_to_pos(it_src_sys->second, it_src_pos->second, sd.sh->speed_km_s,
                                                 it_dest_sys->second, it_dest_pos->second);
    if (!std::isfinite(eta2)) return std::nullopt;

    const double eta_total = eta1 + eta2;

    FreightCandidate c;
    c.kind = FreightAssignmentKind::PickupAndDeliver;
    c.ship_id = sd.ship_id;
    c.source = src_cid;
    c.dest = dest_cid;
    c.restrict_to_discovered = opt.restrict_to_discovered;
    c.load_items = std::move(load_items);
    c.unload_items = std::move(unload_items);
    c.deliver_from_cargo_tons = deliver_from_cargo_total;
    c.eta1 = eta1;
    c.eta2 = eta2;
    c.eta_total = eta_total;
    c.total_tons = total_unload;
    c.eff = eta_total / std::max(1e-9, total_unload);
    return c;
  };

  FreightCandidateWorse worse;

  auto better = [&](const FreightCandidate& a, const FreightCandidate& b) -> bool {
    return worse(b, a);
  };

  auto best_delivery_for_ship = [&](const ShipPlanData& sd) -> std::optional<FreightCandidate> {
    if (sd.used < min_tons) return std::nullopt;
    if (dests_with_needs.empty()) return std::nullopt;

    std::optional<FreightCandidate> best;
    for (Id dest_cid : dests_with_needs) {
      auto cand = make_delivery_candidate(sd, dest_cid);
      if (!cand) continue;
      if (!best || better(*cand, *best)) {
        best = std::move(cand);
      }

      // Also consider topping up from a source colony on the way to the same destination.
      // This is only valid in the delivery phase if we actually deliver meaningful on-board cargo.
      for (Id src_cid : colony_ids) {
        if (exportable_by_colony.find(src_cid) == exportable_by_colony.end()) continue;
        auto topup = make_pickup_candidate(sd, src_cid, dest_cid);
        if (!topup) continue;
        if (topup->deliver_from_cargo_tons < min_tons) continue;
        if (!best || better(*topup, *best)) {
          best = std::move(topup);
        }
      }
    }
    return best;
  };

  auto best_pickup_for_ship = [&](const ShipPlanData& sd) -> std::optional<FreightCandidate> {
    if (sd.free < min_tons) return std::nullopt;
    if (dests_with_needs.empty()) return std::nullopt;
    if (exportable_by_colony.empty()) return std::nullopt;

    std::optional<FreightCandidate> best;

    for (Id dest_cid : dests_with_needs) {
      if (need_minerals_by_colony.find(dest_cid) == need_minerals_by_colony.end()) continue;

      for (Id src_cid : colony_ids) {
        if (exportable_by_colony.find(src_cid) == exportable_by_colony.end()) continue;
        auto cand = make_pickup_candidate(sd, src_cid, dest_cid);
        if (!cand) continue;
        if (!best || better(*cand, *best)) {
          best = std::move(cand);
        }
      }
    }

    return best;
  };

  auto push_candidate = [&](ShipPlanData& sd, FreightCandidate cand,
                            std::priority_queue<FreightCandidate, std::vector<FreightCandidate>, FreightCandidateWorse>& pq) {
    sd.stamp++;
    cand.stamp = sd.stamp;
    pq.push(std::move(cand));
  };

  // --- Planning: globally greedy (best next assignment across all ships), with lazy PQ updates ---
  std::unordered_set<Id> assigned;
  assigned.reserve(ships.size() * 2 + 8);

  out.assignments.reserve(ships.size());

  // Phase 1: deliver existing cargo first (does not consume exportable, only missing).
  {
    std::priority_queue<FreightCandidate, std::vector<FreightCandidate>, FreightCandidateWorse> pq;

    for (auto& sd : ships) {
      if (sd.used < min_tons) continue;
      auto best_cand = best_delivery_for_ship(sd);
      if (best_cand) push_candidate(sd, std::move(*best_cand), pq);
    }

    int safety = 0;
    const int safety_limit = 500000;

    while (!pq.empty() && safety++ < safety_limit) {
      FreightCandidate cand = pq.top();
      pq.pop();

      auto it = ship_index.find(cand.ship_id);
      if (it == ship_index.end()) continue;
      ShipPlanData& sd = ships[it->second];

      if (assigned.find(cand.ship_id) != assigned.end()) continue;
      if (cand.stamp != sd.stamp) continue;  // stale

      auto best_now = best_delivery_for_ship(sd);
      if (!best_now) continue;

      // If another candidate is currently better, reinsert updated best for this ship.
      if (!pq.empty() && worse(*best_now, pq.top())) {
        push_candidate(sd, std::move(*best_now), pq);
        continue;
      }

      // Commit.
      FreightAssignment asg;
      asg.kind = best_now->kind;
      asg.ship_id = best_now->ship_id;
      asg.source_colony_id = best_now->source;
      asg.dest_colony_id = best_now->dest;
      asg.restrict_to_discovered = best_now->restrict_to_discovered;
      asg.items = std::move(best_now->unload_items);

      // Construct explicit route stops so we can support load/unload quantities
      // that differ (partially-loaded ships).
      asg.stops.clear();
      if (!best_now->load_items.empty() && asg.source_colony_id != kInvalidId) {
        FreightStop s;
        s.colony_id = asg.source_colony_id;
        s.actions.reserve(best_now->load_items.size());
        for (const auto& li : best_now->load_items) {
          if (li.tons < min_tons) continue;
          FreightStopAction a;
          a.kind = FreightStopActionKind::Load;
          a.mineral = li.mineral;
          a.tons = li.tons;
          s.actions.push_back(std::move(a));
        }
        if (!s.actions.empty()) asg.stops.push_back(std::move(s));
      }
      {
        FreightStop d;
        d.colony_id = asg.dest_colony_id;
        d.actions.reserve(asg.items.size());
        for (const auto& ui : asg.items) {
          if (ui.tons < min_tons) continue;
          FreightStopAction a;
          a.kind = FreightStopActionKind::Unload;
          a.mineral = ui.mineral;
          a.tons = ui.tons;
          a.reason = ui.reason;
          d.actions.push_back(std::move(a));
        }
        if (!d.actions.empty()) asg.stops.push_back(std::move(d));
      }

      asg.eta_to_source_days = best_now->eta1;
      asg.eta_to_dest_days = best_now->eta2;
      asg.eta_total_days = best_now->eta_total;
      asg.note = (asg.kind == FreightAssignmentKind::DeliverCargo) ? "Deliver existing cargo" : "Top up + deliver";

      for (const auto& it_item : asg.items) {
        dec_missing(asg.dest_colony_id, it_item.mineral, it_item.tons);
      }
      for (const auto& it_item : best_now->load_items) {
        dec_exportable(asg.source_colony_id, it_item.mineral, it_item.tons);
      }

      assigned.insert(asg.ship_id);
      out.assignments.push_back(std::move(asg));
    }
  }

  // Phase 2: pickup + deliver (ships must have free capacity; they may already
  // carry cargo, which will be delivered opportunistically when useful).
  {
    std::priority_queue<FreightCandidate, std::vector<FreightCandidate>, FreightCandidateWorse> pq;

    for (auto& sd : ships) {
      if (assigned.find(sd.ship_id) != assigned.end()) continue;
      if (sd.free < min_tons) continue;

      auto best_cand = best_pickup_for_ship(sd);
      if (best_cand) push_candidate(sd, std::move(*best_cand), pq);
    }

    int safety = 0;
    const int safety_limit = 1000000;

    while (!pq.empty() && safety++ < safety_limit) {
      FreightCandidate cand = pq.top();
      pq.pop();

      auto it = ship_index.find(cand.ship_id);
      if (it == ship_index.end()) continue;
      ShipPlanData& sd = ships[it->second];

      if (assigned.find(cand.ship_id) != assigned.end()) continue;
      if (cand.stamp != sd.stamp) continue;  // stale

      auto best_now = best_pickup_for_ship(sd);
      if (!best_now) continue;

      // If another candidate is currently better, reinsert updated best for this ship.
      if (!pq.empty() && worse(*best_now, pq.top())) {
        push_candidate(sd, std::move(*best_now), pq);
        continue;
      }

      // Commit.
      FreightAssignment asg;
      asg.kind = best_now->kind;
      asg.ship_id = best_now->ship_id;
      asg.source_colony_id = best_now->source;
      asg.dest_colony_id = best_now->dest;
      asg.restrict_to_discovered = best_now->restrict_to_discovered;
      asg.items = std::move(best_now->unload_items);

      // Explicit stop-by-stop route (supports mixed-cargo top-ups).
      asg.stops.clear();
      if (!best_now->load_items.empty() && asg.source_colony_id != kInvalidId) {
        FreightStop s;
        s.colony_id = asg.source_colony_id;
        s.actions.reserve(best_now->load_items.size());
        for (const auto& li : best_now->load_items) {
          if (li.tons < min_tons) continue;
          FreightStopAction a;
          a.kind = FreightStopActionKind::Load;
          a.mineral = li.mineral;
          a.tons = li.tons;
          s.actions.push_back(std::move(a));
        }
        if (!s.actions.empty()) asg.stops.push_back(std::move(s));
      }
      {
        FreightStop d;
        d.colony_id = asg.dest_colony_id;
        d.actions.reserve(asg.items.size());
        for (const auto& ui : asg.items) {
          if (ui.tons < min_tons) continue;
          FreightStopAction a;
          a.kind = FreightStopActionKind::Unload;
          a.mineral = ui.mineral;
          a.tons = ui.tons;
          a.reason = ui.reason;
          d.actions.push_back(std::move(a));
        }
        if (!d.actions.empty()) asg.stops.push_back(std::move(d));
      }

      asg.eta_to_source_days = best_now->eta1;
      asg.eta_to_dest_days = best_now->eta2;
      asg.eta_total_days = best_now->eta_total;
      asg.note = (best_now->deliver_from_cargo_tons >= min_tons) ? "Pickup + deliver (mixed cargo)" : "Pickup + deliver";

      for (const auto& it_item : asg.items) {
        dec_missing(asg.dest_colony_id, it_item.mineral, it_item.tons);
      }
      for (const auto& it_item : best_now->load_items) {
        dec_exportable(asg.source_colony_id, it_item.mineral, it_item.tons);
      }

      assigned.insert(asg.ship_id);
      out.assignments.push_back(std::move(asg));
    }
  }

  out.ok = true;

  // Message / summary.
  if (out.message.empty()) {
    if (out.assignments.empty()) {
      out.message = "No matching freight tasks.";
    } else {
      out.message = "OK";
    }
  } else {
    // Preserve any earlier warning (e.g. truncation) but add a small summary.
    out.message += " (" + std::to_string(out.assignments.size()) + " assignments)";
  }

  return out;
}

bool apply_freight_assignment(Simulation& sim, const FreightAssignment& asg, bool clear_existing_orders) {
  if (asg.ship_id == kInvalidId) return false;

  const bool use_stops = !asg.stops.empty();
  if (!use_stops) {
    if (asg.dest_colony_id == kInvalidId) return false;
    if (asg.items.empty()) return false;
  } else {
    bool has_any_action = false;
    for (const auto& stop : asg.stops) {
      if (stop.colony_id == kInvalidId) return false;
      for (const auto& act : stop.actions) {
        if (act.tons > 0.0 && !act.mineral.empty()) {
          has_any_action = true;
          break;
        }
      }
    }
    if (!has_any_action) return false;
  }

  bool ok = true;

  if (clear_existing_orders) {
    ok = ok && sim.clear_orders(asg.ship_id);
  }

  if (use_stops) {
    for (const auto& stop : asg.stops) {
      for (const auto& act : stop.actions) {
        if (act.tons <= 0.0 || act.mineral.empty()) continue;
        if (act.kind == FreightStopActionKind::Load) {
          ok = ok && sim.issue_load_mineral(asg.ship_id, stop.colony_id, act.mineral, act.tons,
                                            asg.restrict_to_discovered);
        } else {
          ok = ok && sim.issue_unload_mineral(asg.ship_id, stop.colony_id, act.mineral, act.tons,
                                              asg.restrict_to_discovered);
        }
      }
    }
  } else {
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
