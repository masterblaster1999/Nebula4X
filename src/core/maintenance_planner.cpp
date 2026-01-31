#include "nebula4x/core/maintenance_planner.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "nebula4x/core/orders.h"
#include "nebula4x/core/simulation.h"

namespace nebula4x {

namespace {

double clamp_nonneg(double v) {
  if (!std::isfinite(v)) return 0.0;
  return std::max(0.0, v);
}

bool ship_orders_empty(const GameState& st, Id ship_id) {
  auto it = st.ship_orders.find(ship_id);
  if (it == st.ship_orders.end()) return true;
  return ship_orders_is_idle_for_automation(it->second);
}

double get_mineral_amount(const Colony& c, const std::string& key) {
  auto it = c.minerals.find(key);
  if (it == c.minerals.end()) return 0.0;
  return std::max(0.0, it->second);
}

double get_cargo_amount(const Ship& sh, const std::string& key) {
  auto it = sh.cargo.find(key);
  if (it == sh.cargo.end()) return 0.0;
  return std::max(0.0, it->second);
}

struct MaintCol {
  Id colony_id{kInvalidId};
  Id body_id{kInvalidId};
  Id system_id{kInvalidId};
  Vec2 pos_mkm{0.0, 0.0};

  bool owned_by_faction{false};
  bool has_shipyard{false};

  double available_supplies{0.0};
  double remaining_supplies{0.0};
};

double estimate_travel_eta_days(const Simulation& sim, const Ship& sh, Id planning_faction_id, const MaintCol& col,
                                bool restrict_to_discovered) {
  if (sh.system_id == kInvalidId || col.system_id == kInvalidId) return std::numeric_limits<double>::infinity();

  // If already in docking range (same system), treat as 0 travel regardless of speed.
  if (sh.system_id == col.system_id) {
    const double dist = (sh.position_mkm - col.pos_mkm).length();
    if (dist <= std::max(0.0, sim.cfg().docking_range_mkm) + 1e-9) return 0.0;
  }

  if (sh.speed_km_s <= 1e-9) return std::numeric_limits<double>::infinity();

  const auto plan =
      sim.plan_jump_route_from_pos(sh.system_id, sh.position_mkm, planning_faction_id, sh.speed_km_s, col.system_id,
                                   restrict_to_discovered, col.pos_mkm);
  if (!plan) return std::numeric_limits<double>::infinity();
  return std::max(0.0, plan->total_eta_days);
}

}  // namespace

MaintenancePlannerResult compute_maintenance_plan(const Simulation& sim, Id faction_id, const MaintenancePlannerOptions& opt) {
  MaintenancePlannerResult out;
  const GameState& st = sim.state();

  if (faction_id == kInvalidId || st.factions.find(faction_id) == st.factions.end()) {
    out.ok = false;
    out.message = "Invalid faction id.";
    return out;
  }

  const auto& cfg = sim.cfg();
  if (!cfg.enable_ship_maintenance) {
    out.ok = false;
    out.message = "Ship maintenance is disabled in this scenario (cfg.enable_ship_maintenance = false).";
    return out;
  }

  const std::string res = cfg.ship_maintenance_resource_id;
  out.resource_id = res;
  if (res.empty()) {
    out.ok = false;
    out.message = "Ship maintenance is enabled, but cfg.ship_maintenance_resource_id is empty.";
    return out;
  }

  const double per_ton = clamp_nonneg(cfg.ship_maintenance_tons_per_day_per_mass_ton);
  const double rec = clamp_nonneg(cfg.ship_maintenance_recovery_per_day);

  if (rec <= 1e-12 && per_ton <= 1e-12) {
    out.ok = false;
    out.message = "Ship maintenance has no configured recovery or consumption (rec and per_ton are both ~0); planner disabled.";
    return out;
  }

  const double breakdown_start = std::clamp(cfg.ship_maintenance_breakdown_start_fraction, 0.0, 1.0);
  const double breakdown_rate0 = clamp_nonneg(cfg.ship_maintenance_breakdown_rate_per_day_at_zero);
  const double breakdown_exponent = std::max(0.1, cfg.ship_maintenance_breakdown_exponent);

  const double thr = std::clamp(opt.threshold_fraction, 0.0, 1.0);
  const double target = std::clamp(opt.target_fraction, 0.0, 1.0);
  const double reserve_frac = std::clamp(opt.reserve_buffer_fraction, 0.0, 0.95);

  // --- 1) Gather candidate maintenance colonies ---
  std::vector<MaintCol> cols;
  cols.reserve(std::min<int>(static_cast<int>(st.colonies.size()), std::max(0, opt.max_colonies)));

  std::vector<Id> colony_ids;
  colony_ids.reserve(st.colonies.size());
  for (const auto& [cid, _] : st.colonies) colony_ids.push_back(cid);
  std::sort(colony_ids.begin(), colony_ids.end());

  for (Id cid : colony_ids) {
    if (static_cast<int>(cols.size()) >= std::max(0, opt.max_colonies)) {
      out.truncated = true;
      break;
    }

    const Colony* c = find_ptr(st.colonies, cid);
    if (!c) continue;

    const bool owned = (c->faction_id == faction_id);
    if (!owned) {
      if (!opt.include_trade_partner_colonies) continue;
      if (!sim.are_factions_trade_partners(faction_id, c->faction_id)) continue;
    }

    const Body* b = find_ptr(st.bodies, c->body_id);
    if (!b) continue;
    if (b->system_id == kInvalidId) continue;
    if (!find_ptr(st.systems, b->system_id)) continue;

    if (opt.restrict_to_discovered && !sim.is_system_discovered_by_faction(faction_id, b->system_id)) continue;

    // Colony must have some supplies (after reserving buffer) unless planner is in "allow empty" mode.
    const double avail = get_mineral_amount(*c, res);
    const double eff_avail = std::max(0.0, avail * (1.0 - reserve_frac));
    if (opt.require_supplies_available && eff_avail <= 1e-9) continue;

    MaintCol mc;
    mc.colony_id = cid;
    mc.body_id = b->id;
    mc.system_id = b->system_id;
    mc.pos_mkm = b->position_mkm;
    mc.owned_by_faction = owned;
    mc.has_shipyard = [&]() {
      auto it = c->installations.find("shipyard");
      if (it == c->installations.end()) return false;
      return it->second > 0;
    }();
    mc.available_supplies = eff_avail;
    mc.remaining_supplies = eff_avail;
    cols.push_back(mc);
  }

  // --- 2) Gather ships that need maintenance ---
  struct ShipNeed {
    Id ship_id{kInvalidId};
    double condition{1.0};
    double target_condition{1.0};
    double days_needed{0.0};
    double supplies_per_day{0.0};
    double supplies_total{0.0};
    double ship_cargo_supplies{0.0};
    bool critical{false};

    double breakdown_rate_per_day{0.0};
    double breakdown_p_per_day{0.0};
  };

  std::vector<ShipNeed> needs;
  needs.reserve(std::min<int>(static_cast<int>(st.ships.size()), std::max(0, opt.max_ships)));

  std::vector<Id> ship_ids;
  ship_ids.reserve(st.ships.size());
  for (const auto& [sid, _] : st.ships) ship_ids.push_back(sid);
  std::sort(ship_ids.begin(), ship_ids.end());

  for (Id sid : ship_ids) {
    if (static_cast<int>(needs.size()) >= std::max(0, opt.max_ships)) {
      out.truncated = true;
      break;
    }
    const Ship* sh = find_ptr(st.ships, sid);
    if (!sh) continue;
    if (sh->faction_id != faction_id) continue;
    if (sh->system_id == kInvalidId) continue;
    if (!find_ptr(st.systems, sh->system_id)) continue;
    if (opt.require_idle_ships && !ship_orders_empty(st, sid)) continue;
    if (opt.exclude_fleet_ships && sim.fleet_for_ship(sid) != kInvalidId) continue;

    const ShipDesign* d = sim.find_design(sh->design_id);
    if (!d) continue;

    const double cond = std::clamp(sh->maintenance_condition, 0.0, 1.0);
    if (cond + 1e-9 >= thr) continue;

    ShipNeed sn;
    sn.ship_id = sid;
    sn.condition = cond;
    sn.target_condition = std::max(cond, target);

    if (sn.target_condition > cond + 1e-9 && rec > 1e-12) {
      sn.days_needed = (sn.target_condition - cond) / rec;
    } else {
      sn.days_needed = 0.0;
    }

    sn.supplies_per_day = std::max(0.0, d->mass_tons) * per_ton;
    sn.supplies_total = sn.supplies_per_day * std::max(0.0, sn.days_needed);
    sn.ship_cargo_supplies = get_cargo_amount(*sh, res);

    sn.critical = (cond + 1e-9 < breakdown_start);

    if (sn.critical && breakdown_start > 1e-9 && breakdown_rate0 > 1e-12) {
      const double x = std::clamp((breakdown_start - cond) / breakdown_start, 0.0, 1.0);
      if (x > 1e-9) {
        sn.breakdown_rate_per_day = breakdown_rate0 * std::pow(x, breakdown_exponent);
        sn.breakdown_p_per_day = 1.0 - std::exp(-sn.breakdown_rate_per_day);
      }
    }

    needs.push_back(sn);
  }

  // No ships need maintenance.
  if (needs.empty()) {
    out.ok = true;
    out.message = "No ships require maintenance (all above threshold).";
    // Still return colonies for UI visibility.
    out.colonies.reserve(cols.size());
    for (const auto& mc : cols) {
      MaintenanceColonyPlan cp;
      cp.colony_id = mc.colony_id;
      cp.body_id = mc.body_id;
      cp.system_id = mc.system_id;
      cp.owned_by_faction = mc.owned_by_faction;
      cp.has_shipyard = mc.has_shipyard;
      cp.available_supplies_tons = mc.available_supplies;
      cp.remaining_supplies_tons = mc.remaining_supplies;
      out.colonies.push_back(cp);
    }
    return out;
  }

  // Sort worst condition first.
  std::sort(needs.begin(), needs.end(), [](const ShipNeed& a, const ShipNeed& b) {
    if (a.condition != b.condition) return a.condition < b.condition;
    return a.ship_id < b.ship_id;
  });

  // Helper to check whether any shipyard colony exists in candidate list.
  const bool any_shipyard_colony = [&]() {
    for (const auto& mc : cols) {
      if (mc.has_shipyard) return true;
    }
    return false;
  }();

  // --- 3) Assign ships to colonies (greedy by severity) ---
  out.assignments.clear();
  out.assignments.reserve(needs.size());

  // Keep a per-colony reserved count for final summary.
  std::unordered_map<Id, int> assigned_count;
  std::unordered_map<Id, double> reserved_supplies;
  assigned_count.reserve(cols.size() * 2 + 8);
  reserved_supplies.reserve(cols.size() * 2 + 8);

  for (const ShipNeed& sn : needs) {
    MaintenanceAssignment asg;
    asg.ship_id = sn.ship_id;
    asg.restrict_to_discovered = opt.restrict_to_discovered;
    asg.start_condition = sn.condition;
    asg.target_condition = sn.target_condition;
    asg.breakdown_rate_per_day = sn.breakdown_rate_per_day;
    asg.breakdown_p_per_day = sn.breakdown_p_per_day;
    asg.supplies_per_day_tons = sn.supplies_per_day;
    asg.supplies_needed_total_tons = sn.supplies_total;
    asg.supplies_from_ship_cargo_tons = std::min(sn.ship_cargo_supplies, sn.supplies_total);

    const Ship* sh = find_ptr(st.ships, sn.ship_id);
    if (!sh) {
      asg.note = "Ship not found.";
      out.assignments.push_back(asg);
      continue;
    }

    // Remaining required supply to be drawn from colony.
    const double colony_need = std::max(0.0, sn.supplies_total - asg.supplies_from_ship_cargo_tons);

    // If the ship is critical and we require shipyards, limit candidates if possible.
    const bool require_shipyard = opt.prefer_shipyards && opt.require_shipyard_when_critical && sn.critical && any_shipyard_colony;

    struct Cand {
      int idx{-1};
      double eta{0.0};
      double score{0.0};
    };

    std::vector<Cand> cands;
    cands.reserve(std::min<int>(static_cast<int>(cols.size()), std::max(1, opt.max_candidates_per_ship)));

    // Evaluate each colony.
    for (int i = 0; i < static_cast<int>(cols.size()); ++i) {
      const auto& mc = cols[i];
      if (require_shipyard && !mc.has_shipyard) continue;

      // Supply feasibility.
      const double remain = mc.remaining_supplies;
      if (opt.require_supplies_available && colony_need > remain + 1e-9) continue;

      const double eta = estimate_travel_eta_days(sim, *sh, faction_id, mc, opt.restrict_to_discovered);
      if (!std::isfinite(eta)) continue;

      // Soft preferences.
      double score = eta;

      // Prefer shipyards slightly even for non-critical ships (suppresses failures while docked).
      if (opt.prefer_shipyards && !mc.has_shipyard) score += 5.0;

      // Prefer own colonies over trade partners (small bias).
      if (!mc.owned_by_faction) score += 1.0;

      // Prefer colonies with more remaining supply (reduces conflicts / starvation).
      if (colony_need > 1e-9) {
        const double margin = std::max(0.0, remain - colony_need);
        score -= std::min(2.0, margin / std::max(1.0, colony_need));
      }

      cands.push_back(Cand{i, eta, score});
    }

    if (cands.empty() && require_shipyard) {
      // Relax shipyard requirement if it made planning impossible.
      for (int i = 0; i < static_cast<int>(cols.size()); ++i) {
        const auto& mc = cols[i];
        const double remain = mc.remaining_supplies;
        if (opt.require_supplies_available && colony_need > remain + 1e-9) continue;

        const double eta = estimate_travel_eta_days(sim, *sh, faction_id, mc, opt.restrict_to_discovered);
        if (!std::isfinite(eta)) continue;

        double score = eta;
        if (opt.prefer_shipyards && !mc.has_shipyard) score += 5.0;
        if (!mc.owned_by_faction) score += 1.0;
        cands.push_back(Cand{i, eta, score});
      }
    }

    if (cands.empty()) {
      asg.note = cols.empty() ? "No eligible maintenance colonies." : "No eligible maintenance colony can satisfy supplies/route constraints.";
      out.assignments.push_back(asg);
      continue;
    }

    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
      if (a.score != b.score) return a.score < b.score;
      if (a.eta != b.eta) return a.eta < b.eta;
      return a.idx < b.idx;
    });
    if (static_cast<int>(cands.size()) > std::max(1, opt.max_candidates_per_ship)) {
      cands.resize(std::max(1, opt.max_candidates_per_ship));
    }

    const auto best = cands.front();
    const auto& mc = cols[best.idx];

    asg.target_colony_id = mc.colony_id;
    asg.target_has_shipyard = mc.has_shipyard;
    asg.target_owned_by_faction = mc.owned_by_faction;
    asg.travel_eta_days = best.eta;
    asg.start_days = asg.travel_eta_days;
    asg.maintenance_days = std::max(0.0, sn.days_needed);
    asg.finish_days = asg.start_days + asg.maintenance_days;
    asg.supplies_from_colony_tons = colony_need;

    if (asg.breakdown_rate_per_day > 1e-12 && std::isfinite(asg.travel_eta_days) && asg.travel_eta_days > 1e-9) {
      // Poisson: P(at least one breakdown in travel time) = 1 - exp(-lambda * t).
      asg.breakdown_p_during_travel = 1.0 - std::exp(-asg.breakdown_rate_per_day * asg.travel_eta_days);
      asg.breakdown_p_during_travel = std::clamp(asg.breakdown_p_during_travel, 0.0, 1.0);
    }

    // Reserve supply.
    if (asg.target_colony_id != kInvalidId && colony_need > 1e-9) {
      cols[best.idx].remaining_supplies = std::max(0.0, cols[best.idx].remaining_supplies - colony_need);
      reserved_supplies[asg.target_colony_id] += colony_need;
    }
    if (asg.target_colony_id != kInvalidId) {
      assigned_count[asg.target_colony_id] += 1;
    }

    // Notes.
    if (sn.critical && !asg.target_has_shipyard && opt.prefer_shipyards) {
      asg.note = "Critical maintenance (< breakdown threshold): no shipyard colony available; failures may continue.";
    }

    out.assignments.push_back(asg);
  }

  // --- 4) Build colony plans summary ---
  out.colonies.clear();
  out.colonies.reserve(cols.size());
  for (const auto& mc : cols) {
    MaintenanceColonyPlan cp;
    cp.colony_id = mc.colony_id;
    cp.body_id = mc.body_id;
    cp.system_id = mc.system_id;
    cp.owned_by_faction = mc.owned_by_faction;
    cp.has_shipyard = mc.has_shipyard;
    cp.available_supplies_tons = mc.available_supplies;
    cp.remaining_supplies_tons = mc.remaining_supplies;
    cp.assigned_ship_count = assigned_count[mc.colony_id];
    cp.reserved_supplies_tons = reserved_supplies[mc.colony_id];
    if (opt.require_supplies_available && mc.available_supplies <= 1e-9) {
      cp.note = "No available supplies";
    }
    out.colonies.push_back(cp);
  }

  // --- 5) Final message ---
  int planned = 0;
  int unplanned = 0;
  for (const auto& a : out.assignments) {
    if (a.target_colony_id == kInvalidId) {
      ++unplanned;
    } else {
      ++planned;
    }
  }

  out.ok = true;
  if (unplanned > 0) {
    out.message = "Maintenance plan: " + std::to_string(planned) + " assigned, " + std::to_string(unplanned) +
                  " unassigned. Resource: " + res;
  } else {
    out.message = "Maintenance plan: " + std::to_string(planned) + " ships assigned. Resource: " + res;
  }

  return out;
}

bool apply_maintenance_assignment(Simulation& sim, const MaintenanceAssignment& asg, bool clear_existing_orders,
                                 bool use_smart_travel) {
  if (asg.ship_id == kInvalidId) return false;
  if (asg.target_colony_id == kInvalidId) return false;

  auto& st = sim.state();
  const Ship* sh = find_ptr(st.ships, asg.ship_id);
  if (!sh) return false;

  const Colony* c = find_ptr(st.colonies, asg.target_colony_id);
  if (!c) return false;
  const Body* b = find_ptr(st.bodies, c->body_id);
  if (!b) return false;
  if (b->system_id == kInvalidId) return false;

  // If the ship is already in docking range of the target body, we can skip travel orders.
  if (sh->system_id == b->system_id) {
    const double dist = (sh->position_mkm - b->position_mkm).length();
    if (dist <= std::max(0.0, sim.cfg().docking_range_mkm) + 1e-9) {
      if (clear_existing_orders) {
        if (!sim.clear_orders(asg.ship_id)) return false;
      }
      auto& q = st.ship_orders[asg.ship_id].queue;
      q.push_back(OrbitBody{b->id, -1});
      return true;
    }
  }

  if (clear_existing_orders) {
    if (!sim.clear_orders(asg.ship_id)) return false;
  }

  const bool travel_ok = use_smart_travel
                             ? sim.issue_travel_to_system_smart(asg.ship_id, b->system_id, asg.restrict_to_discovered,
                                                                b->position_mkm)
                             : sim.issue_travel_to_system(asg.ship_id, b->system_id, asg.restrict_to_discovered,
                                                          b->position_mkm);
  if (!travel_ok) return false;

  // After arrival, orbit the colony indefinitely to remain in docking range for maintenance recovery.
  auto& q = st.ship_orders[asg.ship_id].queue;
  q.push_back(OrbitBody{b->id, -1});
  return true;
}

bool apply_maintenance_plan(Simulation& sim, const MaintenancePlannerResult& plan, bool clear_existing_orders,
                            bool use_smart_travel) {
  bool ok = true;
  for (const auto& asg : plan.assignments) {
    if (asg.target_colony_id == kInvalidId) continue;
    if (!apply_maintenance_assignment(sim, asg, clear_existing_orders, use_smart_travel)) ok = false;
  }
  return ok;
}

}  // namespace nebula4x
