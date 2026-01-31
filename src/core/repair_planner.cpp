#include "nebula4x/core/repair_planner.h"

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

int priority_rank(RepairPriority p) {
  switch (p) {
    case RepairPriority::High:
      return 0;
    case RepairPriority::Normal:
      return 1;
    case RepairPriority::Low:
      return 2;
    default:
      return 1;
  }
}

bool ship_orders_empty(const GameState& st, Id ship_id) {
  auto it = st.ship_orders.find(ship_id);
  if (it == st.ship_orders.end()) return true;
  return ship_orders_is_idle_for_automation(it->second);
}

double clamp_nonneg(double v) {
  if (!std::isfinite(v)) return 0.0;
  return std::max(0.0, v);
}

struct Yard {
  Id colony_id{kInvalidId};
  Id body_id{kInvalidId};
  Id system_id{kInvalidId};
  Vec2 pos_mkm{0.0, 0.0};

  bool owned_by_faction{false};

  int shipyards{0};
  double nominal_cap_hp_per_day{0.0};
  double effective_cap_hp_per_day{0.0};
  double blockade_mult{1.0};
  double mineral_mult{1.0};
};

double get_mineral_amount(const Colony& c, const std::string& key) {
  auto it = c.minerals.find(key);
  if (it == c.minerals.end()) return 0.0;
  return std::max(0.0, it->second);
}

double mineral_repair_cap_hp_per_day(const Simulation& sim, const Colony& c) {
  const auto& cfg = sim.cfg();

  // Mineral cost per repaired HP-equivalent.
  const double dur_cost = clamp_nonneg(cfg.repair_duranium_per_hp);
  const double neu_cost = clamp_nonneg(cfg.repair_neutronium_per_hp);

  if (dur_cost <= 1e-12 && neu_cost <= 1e-12) return std::numeric_limits<double>::infinity();

  const double dur_avail = get_mineral_amount(c, "Duranium");
  const double neu_avail = get_mineral_amount(c, "Neutronium");

  double cap_by_dur = std::numeric_limits<double>::infinity();
  double cap_by_neu = std::numeric_limits<double>::infinity();
  if (dur_cost > 1e-12) cap_by_dur = dur_avail / dur_cost;
  if (neu_cost > 1e-12) cap_by_neu = neu_avail / neu_cost;

  return std::min(cap_by_dur, cap_by_neu);
}

double compute_yard_capacity(const Simulation& sim, const Colony& c, int shipyards, const RepairPlannerOptions& opt,
                            double& out_blockade_mult, double& out_mineral_mult) {
  const auto& cfg = sim.cfg();
  const double base_per_yard = clamp_nonneg(cfg.repair_hp_per_day_per_shipyard);
  const double nominal = base_per_yard * static_cast<double>(std::max(0, shipyards));

  out_blockade_mult = 1.0;
  out_mineral_mult = 1.0;

  double eff = nominal;

  if (opt.include_blockade_multiplier && cfg.enable_blockades) {
    out_blockade_mult = std::clamp(sim.blockade_output_multiplier_for_colony(c.id), 0.0, 1.0);
    eff *= out_blockade_mult;
  }

  const double eff_pre_mineral = eff;

  if (opt.apply_mineral_limits) {
    const double mineral_cap = mineral_repair_cap_hp_per_day(sim, c);
    if (std::isfinite(mineral_cap)) {
      eff = std::min(eff, mineral_cap);
      if (eff_pre_mineral > 1e-9) out_mineral_mult = std::clamp(eff / eff_pre_mineral, 0.0, 1.0);
    }
  }

  return eff;
}

double ship_missing_hull_hp(const Ship& sh, const ShipDesign* d) {
  const double max_hp = (d ? std::max(0.0, d->max_hp) : std::max(0.0, sh.hp));
  const double hp = std::clamp(sh.hp, 0.0, max_hp);
  return std::max(0.0, max_hp - hp);
}

double ship_missing_subsystem_hp_equiv(const Simulation& sim, const Ship& sh, const ShipDesign* d) {
  const auto& cfg = sim.cfg();
  const double equiv = clamp_nonneg(cfg.ship_subsystem_repair_hp_equiv_per_integrity);
  if (equiv <= 1e-12) return 0.0;

  const double max_hp = (d ? std::max(0.0, d->max_hp) : std::max(0.0, sh.hp));
  const double e = std::clamp(sh.engines_integrity, 0.0, 1.0);
  const double s = std::clamp(sh.sensors_integrity, 0.0, 1.0);
  const double w = std::clamp(sh.weapons_integrity, 0.0, 1.0);
  const double c = std::clamp(sh.shields_integrity, 0.0, 1.0);

  const double deficit = (1.0 - e) + (1.0 - s) + (1.0 - w) + (1.0 - c);
  if (deficit <= 1e-12) return 0.0;
  return deficit * max_hp * equiv;
}

double estimate_travel_eta_days(const Simulation& sim, const Ship& sh, Id planning_faction_id, const Yard& yard,
                                bool restrict_to_discovered) {
  if (sh.system_id == kInvalidId || yard.system_id == kInvalidId) return std::numeric_limits<double>::infinity();

  // If already in docking range (same system), treat as 0 travel regardless of speed.
  if (sh.system_id == yard.system_id) {
    const double dist = (sh.position_mkm - yard.pos_mkm).length();
    if (dist <= std::max(0.0, sim.cfg().docking_range_mkm) + 1e-9) return 0.0;
  }

  if (sh.speed_km_s <= 1e-9) return std::numeric_limits<double>::infinity();

  const auto plan =
      sim.plan_jump_route_from_pos(sh.system_id, sh.position_mkm, planning_faction_id, sh.speed_km_s, yard.system_id,
                                   restrict_to_discovered, yard.pos_mkm);
  if (!plan) return std::numeric_limits<double>::infinity();
  return std::max(0.0, plan->total_eta_days);
}

struct TempJob {
  Id ship_id{kInvalidId};
  RepairPriority priority{RepairPriority::Normal};
  double release_days{0.0};
  double proc_days{0.0};
  double work_hp_equiv{0.0};
};

double simulate_finish_time(std::vector<TempJob> jobs, Id target_ship_id) {
  std::sort(jobs.begin(), jobs.end(), [](const TempJob& a, const TempJob& b) {
    const int pa = priority_rank(a.priority);
    const int pb = priority_rank(b.priority);
    if (pa != pb) return pa < pb;
    if (a.release_days != b.release_days) return a.release_days < b.release_days;
    return a.ship_id < b.ship_id;
  });

  double t = 0.0;
  double finish = std::numeric_limits<double>::infinity();
  for (const auto& j : jobs) {
    if (!std::isfinite(j.release_days) || !std::isfinite(j.proc_days)) continue;
    t = std::max(t, std::max(0.0, j.release_days));
    t += std::max(0.0, j.proc_days);
    if (j.ship_id == target_ship_id) finish = t;
  }
  return finish;
}

}  // namespace

RepairPlannerResult compute_repair_plan(const Simulation& sim, Id faction_id, const RepairPlannerOptions& opt) {
  RepairPlannerResult out;

  const GameState& st = sim.state();
  if (faction_id == kInvalidId || st.factions.find(faction_id) == st.factions.end()) {
    out.ok = false;
    out.message = "Invalid faction id.";
    return out;
  }

  // --- 1) Gather candidate repair yards (colonies with shipyards) ---
  std::vector<Yard> yards;
  yards.reserve(std::min<int>(static_cast<int>(st.colonies.size()), std::max(0, opt.max_yards)));

  std::vector<Id> colony_ids;
  colony_ids.reserve(st.colonies.size());
  for (const auto& [cid, _] : st.colonies) colony_ids.push_back(cid);
  std::sort(colony_ids.begin(), colony_ids.end());

  for (Id cid : colony_ids) {
    if (static_cast<int>(yards.size()) >= std::max(0, opt.max_yards)) {
      out.truncated = true;
      break;
    }

    const Colony* c = find_ptr(st.colonies, cid);
    if (!c) continue;

    const bool owned = (c->faction_id == faction_id);
    if (!owned) {
      if (!opt.include_trade_partner_yards) continue;
      if (!sim.are_factions_trade_partners(faction_id, c->faction_id)) continue;
    }

    const int shipyards_count = [&]() {
      auto it = c->installations.find("shipyard");
      if (it == c->installations.end()) return 0;
      return std::max(0, it->second);
    }();
    if (shipyards_count <= 0) continue;

    const Body* b = find_ptr(st.bodies, c->body_id);
    if (!b) continue;
    if (b->system_id == kInvalidId) continue;
    if (!find_ptr(st.systems, b->system_id)) continue;

    if (opt.restrict_to_discovered && !sim.is_system_discovered_by_faction(faction_id, b->system_id)) continue;

    Yard y;
    y.colony_id = cid;
    y.body_id = b->id;
    y.system_id = b->system_id;
    y.pos_mkm = b->position_mkm;
    y.owned_by_faction = owned;
    y.shipyards = shipyards_count;
    y.nominal_cap_hp_per_day = clamp_nonneg(sim.cfg().repair_hp_per_day_per_shipyard) * shipyards_count;
    y.effective_cap_hp_per_day = compute_yard_capacity(sim, *c, shipyards_count, opt, y.blockade_mult, y.mineral_mult);
    yards.push_back(y);
  }

  if (yards.empty()) {
    out.ok = false;
    out.message = "No repair yards found (no colonies with shipyards available to the faction).";
    return out;
  }

  // --- 2) Gather damaged ships ---
  struct ShipEntry {
    Id ship_id{kInvalidId};
    RepairPriority priority{RepairPriority::Normal};
    double missing_hull{0.0};
    double missing_subsys{0.0};
    double work{0.0};

    // Candidate yards for assignment (yard_index, travel_eta).
    std::vector<std::pair<int, double>> candidates;

    // Results.
    int assigned_yard{-1};
  };

  std::vector<Id> ship_ids;
  ship_ids.reserve(st.ships.size());
  for (const auto& [sid, _] : st.ships) ship_ids.push_back(sid);
  std::sort(ship_ids.begin(), ship_ids.end());

  std::vector<ShipEntry> ships;
  ships.reserve(std::min<int>(static_cast<int>(st.ships.size()), std::max(0, opt.max_ships)));

  for (Id sid : ship_ids) {
    if (static_cast<int>(ships.size()) >= std::max(0, opt.max_ships)) {
      out.truncated = true;
      break;
    }

    const Ship* sh = find_ptr(st.ships, sid);
    if (!sh) continue;
    if (sh->faction_id != faction_id) continue;
    if (sh->system_id == kInvalidId) continue;

    if (opt.exclude_fleet_ships && sim.fleet_for_ship(sid) != kInvalidId) continue;
    if (opt.require_idle_ships && !ship_orders_empty(st, sid)) continue;

    const ShipDesign* d = sim.find_design(sh->design_id);

    ShipEntry e;
    e.ship_id = sid;
    e.priority = sh->repair_priority;
    e.missing_hull = ship_missing_hull_hp(*sh, d);
    e.missing_subsys = opt.include_subsystem_repairs ? ship_missing_subsystem_hp_equiv(sim, *sh, d) : 0.0;
    e.work = e.missing_hull + e.missing_subsys;

    if (e.work <= 1e-6) continue;  // not meaningfully damaged

    // Candidate yards.
    e.candidates.reserve(yards.size());
    for (int yi = 0; yi < static_cast<int>(yards.size()); ++yi) {
      const Yard& y = yards[yi];
      if (y.effective_cap_hp_per_day <= 1e-9) continue;  // can't repair (blocked / no minerals)
      const double eta = estimate_travel_eta_days(sim, *sh, faction_id, y, opt.restrict_to_discovered);
      if (!std::isfinite(eta)) continue;
      e.candidates.push_back({yi, eta});
    }

    if (e.candidates.empty()) {
      // Keep it so UI can show "unassigned" ships.
      ships.push_back(e);
      continue;
    }

    std::sort(e.candidates.begin(), e.candidates.end(), [](const auto& a, const auto& b) {
      if (a.second != b.second) return a.second < b.second;
      return a.first < b.first;
    });
    if (static_cast<int>(e.candidates.size()) > std::max(1, opt.max_candidates_per_ship)) {
      e.candidates.resize(std::max(1, opt.max_candidates_per_ship));
    }

    ships.push_back(e);
  }

  // --- 3) Greedy assignment with release-time aware finish-time simulation ---
  std::vector<std::vector<TempJob>> yard_jobs(yards.size());

  // Deterministic order: higher priority first, then larger jobs, then ship id.
  std::sort(ships.begin(), ships.end(), [](const ShipEntry& a, const ShipEntry& b) {
    const int pa = priority_rank(a.priority);
    const int pb = priority_rank(b.priority);
    if (pa != pb) return pa < pb;
    if (a.work != b.work) return a.work > b.work;
    return a.ship_id < b.ship_id;
  });

  for (auto& sh : ships) {
    if (sh.candidates.empty()) continue;

    double best_finish = std::numeric_limits<double>::infinity();
    int best_yard = -1;
    double best_eta = std::numeric_limits<double>::infinity();

    for (const auto& [yi, eta] : sh.candidates) {
      const Yard& y = yards[yi];
      if (y.effective_cap_hp_per_day <= 1e-9) continue;

      TempJob cand;
      cand.ship_id = sh.ship_id;
      cand.priority = sh.priority;
      cand.release_days = eta;
      cand.work_hp_equiv = sh.work;
      cand.proc_days = sh.work / y.effective_cap_hp_per_day;

      // Simulate finish time if we add this job to yard yi.
      std::vector<TempJob> tmp = yard_jobs[yi];
      tmp.push_back(cand);
      const double finish = simulate_finish_time(std::move(tmp), sh.ship_id);
      if (!std::isfinite(finish)) continue;

      bool take = false;
      if (finish + 1e-9 < best_finish) {
        take = true;
      } else if (std::abs(finish - best_finish) <= 1e-9) {
        // Tie-breaks: prefer owned yards, then lower travel, then more capacity.
        const bool owned = y.owned_by_faction;
        const bool best_owned = (best_yard >= 0) ? yards[best_yard].owned_by_faction : false;
        if (owned != best_owned) {
          take = owned;  // prefer owned
        } else if (eta != best_eta) {
          take = eta < best_eta;
        } else if (best_yard >= 0) {
          take = y.effective_cap_hp_per_day > yards[best_yard].effective_cap_hp_per_day;
        }
      }

      if (take) {
        best_finish = finish;
        best_yard = yi;
        best_eta = eta;
      }
    }

    if (best_yard >= 0) {
      sh.assigned_yard = best_yard;

      TempJob job;
      job.ship_id = sh.ship_id;
      job.priority = sh.priority;
      job.release_days = best_eta;
      job.work_hp_equiv = sh.work;
      job.proc_days = sh.work / yards[best_yard].effective_cap_hp_per_day;
      yard_jobs[best_yard].push_back(job);
    }
  }

  // --- 4) Final per-yard schedules (start/finish for each ship) ---
  struct Sched {
    int yard{-1};
    double release{0.0};
    double start{0.0};
    double finish{0.0};
    double proc{0.0};
  };
  std::unordered_map<Id, Sched> sched;
  sched.reserve(ships.size() * 2);

  std::vector<RepairYardPlan> yard_out;
  yard_out.reserve(yards.size());

  for (int yi = 0; yi < static_cast<int>(yards.size()); ++yi) {
    auto jobs = yard_jobs[yi];

    std::sort(jobs.begin(), jobs.end(), [](const TempJob& a, const TempJob& b) {
      const int pa = priority_rank(a.priority);
      const int pb = priority_rank(b.priority);
      if (pa != pb) return pa < pb;
      if (a.release_days != b.release_days) return a.release_days < b.release_days;
      return a.ship_id < b.ship_id;
    });

    double t = 0.0;
    double total_proc = 0.0;
    double backlog = 0.0;

    for (const auto& j : jobs) {
      if (!std::isfinite(j.release_days) || !std::isfinite(j.proc_days)) continue;
      t = std::max(t, std::max(0.0, j.release_days));
      const double start = t;
      t += std::max(0.0, j.proc_days);
      total_proc += std::max(0.0, j.proc_days);
      backlog += std::max(0.0, j.work_hp_equiv);
      sched[j.ship_id] = Sched{yi, j.release_days, start, t, j.proc_days};
    }

    RepairYardPlan yp;
    yp.colony_id = yards[yi].colony_id;
    yp.body_id = yards[yi].body_id;
    yp.system_id = yards[yi].system_id;
    yp.shipyards = yards[yi].shipyards;
    yp.nominal_capacity_hp_per_day = yards[yi].nominal_cap_hp_per_day;
    yp.effective_capacity_hp_per_day = yards[yi].effective_cap_hp_per_day;
    yp.blockade_multiplier = yards[yi].blockade_mult;
    yp.mineral_limit_multiplier = yards[yi].mineral_mult;
    yp.assigned_ship_count = static_cast<int>(jobs.size());
    yp.backlog_hp_equiv = backlog;
    if (yp.effective_capacity_hp_per_day > 1e-9) {
      yp.processing_days = backlog / yp.effective_capacity_hp_per_day;
    } else {
      yp.processing_days = std::numeric_limits<double>::infinity();
    }
    yp.makespan_days = t;
    yp.utilization = (t > 1e-9) ? std::clamp(total_proc / t, 0.0, 1.0) : 0.0;

    yard_out.push_back(yp);
  }

  // --- 5) Emit per-ship assignments ---
  std::vector<RepairAssignment> asgs;
  asgs.reserve(ships.size());

  for (const auto& sh : ships) {
    RepairAssignment a;
    a.ship_id = sh.ship_id;
    a.restrict_to_discovered = opt.restrict_to_discovered;
    a.priority = sh.priority;
    a.missing_hull_hp = sh.missing_hull;
    a.missing_subsystem_hp_equiv = sh.missing_subsys;
    a.total_missing_hp_equiv = sh.work;

    if (sh.assigned_yard < 0) {
      a.target_colony_id = kInvalidId;
      if (sh.candidates.empty()) {
        a.note = "No reachable shipyards (routing failed or zero repair capacity).";
      } else {
        a.note = "No feasible assignment (all candidates filtered or capacity=0).";
      }
      asgs.push_back(std::move(a));
      continue;
    }

    const Yard& y = yards[sh.assigned_yard];
    a.target_colony_id = y.colony_id;

    auto it = sched.find(sh.ship_id);
    if (it != sched.end()) {
      a.travel_eta_days = it->second.release;
      a.start_repair_days = it->second.start;
      a.finish_repair_days = it->second.finish;
      a.queue_wait_days = std::max(0.0, it->second.start - it->second.release);
      a.repair_days = it->second.proc;
    } else {
      // Should not happen, but keep safe.
      a.travel_eta_days = sh.candidates.empty() ? 0.0 : sh.candidates.front().second;
      a.note = "Internal scheduling error.";
    }

    asgs.push_back(std::move(a));
  }

  // Sort yards (most urgent first).
  std::sort(yard_out.begin(), yard_out.end(), [](const RepairYardPlan& a, const RepairYardPlan& b) {
    if (a.makespan_days != b.makespan_days) return a.makespan_days > b.makespan_days;
    return a.colony_id < b.colony_id;
  });

  // Sort assignments (priority then finish time).
  std::sort(asgs.begin(), asgs.end(), [](const RepairAssignment& a, const RepairAssignment& b) {
    const int pa = priority_rank(a.priority);
    const int pb = priority_rank(b.priority);
    if (pa != pb) return pa < pb;
    // Unassigned last.
    const bool ua = (a.target_colony_id == kInvalidId);
    const bool ub = (b.target_colony_id == kInvalidId);
    if (ua != ub) return ub;  // false < true
    if (a.finish_repair_days != b.finish_repair_days) return a.finish_repair_days < b.finish_repair_days;
    return a.ship_id < b.ship_id;
  });

  out.ok = true;
  out.yards = std::move(yard_out);
  out.assignments = std::move(asgs);

  const int n_damaged = static_cast<int>(out.assignments.size());
  if (n_damaged == 0) {
    out.message = "No damaged ships found.";
  } else {
    out.message = "Planned repairs for " + std::to_string(n_damaged) + " damaged ships.";
    if (out.truncated) out.message += " (truncated)";
  }

  return out;
}

bool apply_repair_assignment(Simulation& sim, const RepairAssignment& asg, bool clear_existing_orders,
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

  // After arrival, orbit the colony indefinitely to remain in docking range for repairs.
  auto& q = st.ship_orders[asg.ship_id].queue;
  q.push_back(OrbitBody{b->id, -1});
  return true;
}

bool apply_repair_plan(Simulation& sim, const RepairPlannerResult& plan, bool clear_existing_orders,
                       bool use_smart_travel) {
  bool ok = true;
  for (const auto& asg : plan.assignments) {
    if (asg.target_colony_id == kInvalidId) continue;
    if (!apply_repair_assignment(sim, asg, clear_existing_orders, use_smart_travel)) ok = false;
  }
  return ok;
}

}  // namespace nebula4x
