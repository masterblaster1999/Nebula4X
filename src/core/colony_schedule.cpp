#include "nebula4x/core/colony_schedule.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "nebula4x/core/entities.h"
#include "nebula4x/core/game_state.h"
#include "nebula4x/core/simulation.h"

// Reuse internal helpers for determinism and mining classification.
#include "simulation_internal.h"

namespace nebula4x {

namespace {

using sim_internal::FactionEconomyMultipliers;
using sim_internal::compute_faction_economy_multipliers;
using sim_internal::is_mining_installation;
using sim_internal::sorted_keys;

inline double get_mineral(const std::unordered_map<std::string, double>& m, const std::string& key) {
  auto it = m.find(key);
  if (it == m.end()) return 0.0;
  const double v = it->second;
  if (!std::isfinite(v) || v < 0.0) return 0.0;
  return v;
}

inline void add_mineral(std::unordered_map<std::string, double>& m, const std::string& key, double delta) {
  if (!(delta > 0.0)) return;
  m[key] += delta;
}

inline void sub_mineral(std::unordered_map<std::string, double>& m, const std::string& key, double delta) {
  if (!(delta > 0.0)) return;
  double& v = m[key];
  if (!std::isfinite(v) || v < 0.0) v = 0.0;
  v = std::max(0.0, v - delta);
  if (v <= 1e-9) v = 0.0;
}

// Mirrors Simulation::tick_construction auto-build behavior, but operates on a local Colony copy.
void apply_auto_construction_targets(const Simulation& sim, Colony& colony) {
  if (colony.installation_targets.empty()) return;

  auto target_for = [&](const std::string& inst_id) -> int {
    auto it = colony.installation_targets.find(inst_id);
    if (it == colony.installation_targets.end()) return 0;
    return std::max(0, it->second);
  };

  auto committed_units = [&](const InstallationBuildOrder& ord) -> int {
    // If we're already building the current unit (minerals paid or CP started),
    // treat one unit as committed and do not prune it.
    const bool in_prog = ord.minerals_paid || ord.cp_remaining > 1e-9;
    return in_prog ? 1 : 0;
  };

  // 1) Prune auto-queued orders whose target is now zero/missing.
  for (int i = static_cast<int>(colony.construction_queue.size()) - 1; i >= 0; --i) {
    auto& ord = colony.construction_queue[static_cast<std::size_t>(i)];
    if (!ord.auto_queued) continue;
    if (target_for(ord.installation_id) > 0) continue;

    const int committed = std::min(std::max(0, ord.quantity_remaining), committed_units(ord));
    if (ord.quantity_remaining > committed) {
      ord.quantity_remaining = committed;
    }
    if (ord.quantity_remaining <= 0) {
      colony.construction_queue.erase(colony.construction_queue.begin() + i);
    }
  }

  // 2) Compute pending quantities by installation id, split by manual vs auto.
  std::unordered_map<std::string, int> manual_pending;
  std::unordered_map<std::string, int> auto_pending;
  manual_pending.reserve(colony.construction_queue.size() * 2);
  auto_pending.reserve(colony.construction_queue.size() * 2);

  for (const auto& ord : colony.construction_queue) {
    if (ord.installation_id.empty()) continue;
    const int qty = std::max(0, ord.quantity_remaining);
    if (qty <= 0) continue;
    if (ord.auto_queued) {
      auto_pending[ord.installation_id] += qty;
    } else {
      manual_pending[ord.installation_id] += qty;
    }
  }

  // Sorted keys for determinism.
  std::vector<std::string> ids;
  ids.reserve(colony.installation_targets.size());
  for (const auto& [inst_id, _] : colony.installation_targets) ids.push_back(inst_id);
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

  for (const auto& inst_id : ids) {
    if (inst_id.empty()) continue;
    const int target = target_for(inst_id);
    if (target <= 0) continue;

    const int have = [&]() -> int {
      auto it = colony.installations.find(inst_id);
      return (it == colony.installations.end()) ? 0 : std::max(0, it->second);
    }();

    const int man = manual_pending[inst_id];
    const int aut = auto_pending[inst_id];

    const int required_auto = std::max(0, target - (have + man));

    // 3) Trim excess auto-queued units for this installation id.
    if (aut > required_auto) {
      int remove = aut - required_auto;
      for (int i = static_cast<int>(colony.construction_queue.size()) - 1; i >= 0 && remove > 0; --i) {
        auto& ord = colony.construction_queue[static_cast<std::size_t>(i)];
        if (!ord.auto_queued) continue;
        if (ord.installation_id != inst_id) continue;

        const int committed = std::min(std::max(0, ord.quantity_remaining), committed_units(ord));
        const int cancelable = std::max(0, ord.quantity_remaining - committed);
        if (cancelable <= 0) continue;

        const int take = std::min(cancelable, remove);
        ord.quantity_remaining -= take;
        remove -= take;

        if (ord.quantity_remaining <= 0) {
          colony.construction_queue.erase(colony.construction_queue.begin() + i);
        }
      }
    }

    // 4) Add missing auto-queued units.
    int aut_after = 0;
    for (const auto& ord : colony.construction_queue) {
      if (!ord.auto_queued) continue;
      if (ord.installation_id != inst_id) continue;
      aut_after += std::max(0, ord.quantity_remaining);
    }

    const int missing = std::max(0, required_auto - aut_after);
    if (missing > 0) {
      // Only auto-queue installations the faction can build.
      if (sim.is_installation_buildable_for_faction(colony.faction_id, inst_id)) {
        InstallationBuildOrder ord;
        ord.installation_id = inst_id;
        ord.quantity_remaining = missing;
        ord.auto_queued = true;
        colony.construction_queue.push_back(ord);
      }
    }
  }
}

// Simulate one day of non-mining industry for a single colony.
// Returns true if any mineral stockpile changed.
bool simulate_industry_day(const ContentDB& content, Colony& colony, double industry_mult, double dt_days) {
  if (dt_days <= 0.0) return false;
  if (!(industry_mult > 0.0)) industry_mult = 0.0;

  bool changed = false;

  // Deterministic processing: installation iteration order of unordered_map is unspecified.
  std::vector<std::string> inst_ids;
  inst_ids.reserve(colony.installations.size());
  for (const auto& [inst_id, _] : colony.installations) inst_ids.push_back(inst_id);
  std::sort(inst_ids.begin(), inst_ids.end());

  for (const std::string& inst_id : inst_ids) {
    auto it_count = colony.installations.find(inst_id);
    if (it_count == colony.installations.end()) continue;
    const int count = std::max(0, it_count->second);
    if (count <= 0) continue;

    const auto it_def = content.installations.find(inst_id);
    if (it_def == content.installations.end()) continue;
    const InstallationDef& def = it_def->second;

    // Mining is handled separately against finite deposits.
    if (is_mining_installation(def)) continue;

    if (def.produces_per_day.empty() && def.consumes_per_day.empty()) continue;

    // Compute the fraction of full-rate operation we can support with available inputs.
    double frac = 1.0;
    for (const auto& [mineral, per_day_raw] : def.consumes_per_day) {
      const double per_day = std::max(0.0, per_day_raw);
      if (per_day <= 1e-12) continue;

      const double req = per_day * static_cast<double>(count) * dt_days;
      const double have = get_mineral(colony.minerals, mineral);
      if (req > 1e-12) frac = std::min(frac, have / req);
    }

    frac = std::clamp(frac, 0.0, 1.0);
    if (frac <= 1e-12) continue;

    // Consume inputs first, then produce outputs.
    for (const auto& [mineral, per_day_raw] : def.consumes_per_day) {
      const double per_day = std::max(0.0, per_day_raw);
      if (per_day <= 1e-12) continue;
      const double amt = per_day * static_cast<double>(count) * frac * dt_days;
      if (amt <= 1e-12) continue;
      const double before = get_mineral(colony.minerals, mineral);
      sub_mineral(colony.minerals, mineral, amt);
      const double after = get_mineral(colony.minerals, mineral);
      if (std::fabs(after - before) > 1e-9) changed = true;
    }

    for (const auto& [mineral, per_day_raw] : def.produces_per_day) {
      const double per_day = std::max(0.0, per_day_raw);
      if (per_day <= 1e-12) continue;
      const double amt = per_day * static_cast<double>(count) * frac * industry_mult * dt_days;
      if (amt <= 1e-12) continue;
      const double before = get_mineral(colony.minerals, mineral);
      add_mineral(colony.minerals, mineral, amt);
      const double after = get_mineral(colony.minerals, mineral);
      if (std::fabs(after - before) > 1e-9) changed = true;
    }
  }

  return changed;
}

// Compute one-day mining extraction for the target colony, accounting for finite deposits
// and other colonies on the same body. Returns true if any mineral stockpile changed.
bool simulate_mining_day(const Simulation& sim,
                         const std::unordered_map<Id, FactionEconomyMultipliers>& fac_mult,
                         Id target_colony_id,
                         Colony& target_colony,
                         std::unordered_map<std::string, double>& deposits,
                         double dt_days) {
  if (dt_days <= 0.0) return false;
  if (target_colony.body_id == kInvalidId) return false;

  // Build a list of colonies on the same body.
  std::vector<const Colony*> colonies_on_body;
  colonies_on_body.reserve(sim.state().colonies.size());
  for (const auto& [cid, c] : sim.state().colonies) {
    if (c.body_id != target_colony.body_id) continue;
    // Use the schedule's working colony for the target id; others remain snapshot.
    if (cid == target_colony_id) continue;
    colonies_on_body.push_back(&c);
  }

  // Accumulate total requests per mineral and the target colony's request per mineral.
  std::unordered_map<std::string, double> total_req;
  std::unordered_map<std::string, double> target_req;

  auto mining_mult_for = [&](Id fid) -> double {
    auto it = fac_mult.find(fid);
    if (it == fac_mult.end()) return 1.0;
    return std::max(0.0, it->second.mining);
  };

  auto add_requests_from = [&](const Colony& c, double mining_mult, bool is_target) {
    if (!(mining_mult > 0.0)) return;
    for (const auto& [inst_id, count_raw] : c.installations) {
      const int count = std::max(0, count_raw);
      if (count <= 0) continue;
      auto it_def = sim.content().installations.find(inst_id);
      if (it_def == sim.content().installations.end()) continue;
      const InstallationDef& def = it_def->second;
      if (!is_mining_installation(def)) continue;
      for (const auto& [mineral, per_day_raw] : def.produces_per_day) {
        const double per_day = std::max(0.0, per_day_raw);
        if (per_day <= 1e-12) continue;
        const double req = per_day * static_cast<double>(count) * mining_mult * dt_days;
        if (req <= 1e-12) continue;
        total_req[mineral] += req;
        if (is_target) target_req[mineral] += req;
      }
    }
  };

  add_requests_from(target_colony, mining_mult_for(target_colony.faction_id), true);
  for (const Colony* c : colonies_on_body) {
    add_requests_from(*c, mining_mult_for(c->faction_id), false);
  }

  bool changed = false;

  // Apply extraction against deposits for each mineral requested.
  std::vector<std::string> minerals = sorted_keys(total_req);
  for (const std::string& mineral : minerals) {
    const double req_total = std::max(0.0, total_req[mineral]);
    const double req_target = std::max(0.0, target_req[mineral]);
    if (req_total <= 1e-12 || req_target <= 1e-12) continue;

    auto it_dep = deposits.find(mineral);
    if (it_dep == deposits.end()) {
      // No deposit entry: treat as infinite (legacy/prototype behavior).
      const double before = get_mineral(target_colony.minerals, mineral);
      add_mineral(target_colony.minerals, mineral, req_target);
      const double after = get_mineral(target_colony.minerals, mineral);
      if (std::fabs(after - before) > 1e-9) changed = true;
      continue;
    }

    const double before_dep = std::max(0.0, it_dep->second);
    if (before_dep <= 1e-12) continue;

    const double take_total = std::min(before_dep, req_total);
    const double ratio = (req_total <= 1e-12) ? 0.0 : (take_total / req_total);
    const double gain = req_target * ratio;

    if (gain > 1e-12) {
      const double before = get_mineral(target_colony.minerals, mineral);
      add_mineral(target_colony.minerals, mineral, gain);
      const double after = get_mineral(target_colony.minerals, mineral);
      if (std::fabs(after - before) > 1e-9) changed = true;
    }

    it_dep->second = std::max(0.0, before_dep - take_total);
    if (it_dep->second <= 1e-9) it_dep->second = 0.0;
  }

  return changed;
}

bool simulate_shipyard_day(const Simulation& sim,
                           Colony& colony,
                           const FactionEconomyMultipliers& mult,
                           const InstallationDef& shipyard_def,
                           int day,
                           std::vector<ColonyScheduleEvent>& events,
                           bool& hard_block,
                           std::string& hard_block_reason) {
  hard_block = false;
  hard_block_reason.clear();

  const int yards = [&]() {
    auto it = colony.installations.find("shipyard");
    if (it == colony.installations.end()) return 0;
    return std::max(0, it->second);
  }();

  if (yards <= 0) return false;
  if (shipyard_def.build_rate_tons_per_day <= 1e-12) return false;
  if (colony.shipyard_queue.empty()) return false;

  double capacity_tons = shipyard_def.build_rate_tons_per_day * static_cast<double>(yards) * std::max(0.0, mult.shipyard);
  if (capacity_tons <= 1e-9) return false;

  bool progressed = false;

  auto max_buildable_tons_by_minerals = [&](double desired_tons) -> double {
    double max_tons = desired_tons;
    for (const auto& [mineral, per_ton_raw] : shipyard_def.build_costs_per_ton) {
      const double per_ton = std::max(0.0, per_ton_raw);
      if (per_ton <= 1e-12) continue;
      const double have = get_mineral(colony.minerals, mineral);
      max_tons = std::min(max_tons, have / per_ton);
    }
    if (!std::isfinite(max_tons)) return 0.0;
    return std::max(0.0, max_tons);
  };

  while (capacity_tons > 1e-9 && !colony.shipyard_queue.empty()) {
    BuildOrder& bo = colony.shipyard_queue.front();
    bo.tons_remaining = std::max(0.0, bo.tons_remaining);
    if (bo.tons_remaining <= 1e-9) {
      colony.shipyard_queue.erase(colony.shipyard_queue.begin());
      continue;
    }

    // Refit orders require the ship to already be docked (we don't simulate ship movement here).
    if (bo.is_refit()) {
      if (!sim.is_ship_docked_at_colony(bo.refit_ship_id, colony.id)) {
        hard_block = true;
        hard_block_reason = "Refit is waiting for ship to be docked";
        return progressed;
      }
    }

    double build_tons = std::min(capacity_tons, bo.tons_remaining);
    if (!shipyard_def.build_costs_per_ton.empty()) {
      build_tons = std::min(build_tons, max_buildable_tons_by_minerals(build_tons));
    }

    if (build_tons <= 1e-9) {
      // Mineral limited.
      return progressed;
    }

    // Pay minerals.
    for (const auto& [mineral, per_ton_raw] : shipyard_def.build_costs_per_ton) {
      const double per_ton = std::max(0.0, per_ton_raw);
      if (per_ton <= 1e-12) continue;
      sub_mineral(colony.minerals, mineral, build_tons * per_ton);
    }

    bo.tons_remaining -= build_tons;
    capacity_tons -= build_tons;
    progressed = true;

    if (bo.tons_remaining > 1e-9) {
      // Still building the current order.
      return progressed;
    }

    // Completed.
    ColonyScheduleEvent ev;
    ev.kind = ColonyScheduleEventKind::ShipyardComplete;
    ev.day = day;
    ev.title = "Shipyard";
    ev.auto_queued = bo.auto_queued;
    if (bo.is_refit()) {
      ev.detail = "Refit -> " + bo.design_id;
    } else {
      ev.detail = "Built " + bo.design_id;
    }
    events.push_back(std::move(ev));

    colony.shipyard_queue.erase(colony.shipyard_queue.begin());
  }

  return progressed;
}

bool simulate_construction_day(const Simulation& sim,
                               Colony& colony,
                               int day,
                               std::vector<ColonyScheduleEvent>& events) {
  double cp_available = sim.construction_points_per_day(colony);
  if (cp_available <= 1e-9) return false;

  // Auto-build targets are managed inside tick_construction, after shipyards.
  apply_auto_construction_targets(sim, colony);

  auto can_pay_minerals = [&](const InstallationDef& def) {
    for (const auto& [mineral, cost] : def.build_costs) {
      const double c = std::max(0.0, cost);
      if (c <= 1e-12) continue;
      const double have = get_mineral(colony.minerals, mineral);
      if (have + 1e-9 < c) return false;
    }
    return true;
  };

  auto pay_minerals = [&](const InstallationDef& def) {
    for (const auto& [mineral, cost] : def.build_costs) {
      const double c = std::max(0.0, cost);
      if (c <= 1e-12) continue;
      sub_mineral(colony.minerals, mineral, c);
    }
  };

  bool progressed = false;

  // Deterministic queue processing: preserve queue order, but allow skipping stalled orders.
  for (std::size_t i = 0; i < colony.construction_queue.size() && cp_available > 1e-9;) {
    auto& ord = colony.construction_queue[i];
    if (ord.quantity_remaining <= 0 || ord.installation_id.empty()) {
      colony.construction_queue.erase(colony.construction_queue.begin() + static_cast<long>(i));
      continue;
    }

    auto it_def = sim.content().installations.find(ord.installation_id);
    if (it_def == sim.content().installations.end()) {
      // Unknown installation, drop.
      colony.construction_queue.erase(colony.construction_queue.begin() + static_cast<long>(i));
      continue;
    }
    const InstallationDef& def = it_def->second;

    // If we haven't started this unit, try to pay minerals to begin.
    if (!ord.minerals_paid) {
      if (!can_pay_minerals(def)) {
        ++i; // stalled, try later orders
        continue;
      }

      pay_minerals(def);
      ord.minerals_paid = true;
      ord.cp_remaining = std::max(0.0, def.construction_cost);
      progressed = true;

      if (def.construction_cost <= 1e-12) {
        // Instant build.
        colony.installations[ord.installation_id] += 1;
        ord.quantity_remaining -= 1;
        ord.minerals_paid = false;
        ord.cp_remaining = 0.0;

        ColonyScheduleEvent ev;
        ev.kind = ColonyScheduleEventKind::ConstructionComplete;
        ev.day = day;
        ev.title = "Construction";
        ev.auto_queued = ord.auto_queued;
        ev.detail = "Built " + ord.installation_id;
        events.push_back(std::move(ev));

        if (ord.quantity_remaining <= 0) {
          colony.construction_queue.erase(colony.construction_queue.begin() + static_cast<long>(i));
        }
        continue;
      }
    }

    // Spend CP on the current unit.
    if (ord.cp_remaining <= 1e-9 && def.construction_cost > 1e-12) {
      // Defensive: restore if missing.
      ord.cp_remaining = def.construction_cost;
    }

    const double spend = std::min(cp_available, std::max(0.0, ord.cp_remaining));
    if (spend > 1e-12) {
      ord.cp_remaining -= spend;
      cp_available -= spend;
      progressed = true;
    }

    if (ord.cp_remaining <= 1e-9 && def.construction_cost > 1e-12) {
      // Unit complete.
      colony.installations[ord.installation_id] += 1;
      ord.quantity_remaining -= 1;
      ord.minerals_paid = false;
      ord.cp_remaining = 0.0;

      ColonyScheduleEvent ev;
      ev.kind = ColonyScheduleEventKind::ConstructionComplete;
      ev.day = day;
      ev.title = "Construction";
      ev.auto_queued = ord.auto_queued;
      ev.detail = "Built " + ord.installation_id;
      events.push_back(std::move(ev));

      if (ord.quantity_remaining <= 0) {
        colony.construction_queue.erase(colony.construction_queue.begin() + static_cast<long>(i));
      }
      continue;
    }

    // Move to next order.
    ++i;
  }

  return progressed;
}

} // namespace

ColonySchedule estimate_colony_schedule(const Simulation& sim, Id colony_id, const ColonyScheduleOptions& opt) {
  ColonySchedule out;
  out.colony_id = colony_id;

  const Colony* c0 = find_ptr(sim.state().colonies, colony_id);
  if (!c0) return out;

  Colony colony = *c0; // working copy
  out.faction_id = colony.faction_id;
  out.minerals_start = colony.minerals;

  // Precompute economy multipliers for all factions (for mining share accuracy).
  std::unordered_map<Id, FactionEconomyMultipliers> fac_mult;
  fac_mult.reserve(sim.state().factions.size());
  for (Id fid : sorted_keys(sim.state().factions)) {
    fac_mult.emplace(fid, compute_faction_economy_multipliers(sim.content(), sim.state().factions.at(fid)));
  }
  const FactionEconomyMultipliers default_mult;
  auto mult_for = [&](Id fid) -> const FactionEconomyMultipliers& {
    auto it = fac_mult.find(fid);
    if (it == fac_mult.end()) return default_mult;
    return it->second;
  };

  const FactionEconomyMultipliers my_mult = mult_for(colony.faction_id);
  out.mining_multiplier = my_mult.mining;
  out.industry_multiplier = my_mult.industry;
  out.construction_multiplier = my_mult.construction;
  out.shipyard_multiplier = my_mult.shipyard;

  out.construction_cp_per_day_start = sim.construction_points_per_day(colony);

  // Shipyard capacity snapshot.
  const InstallationDef* shipyard_def = nullptr;
  if (auto it = sim.content().installations.find("shipyard"); it != sim.content().installations.end()) {
    shipyard_def = &it->second;
  }
  if (shipyard_def) {
    const int yards = [&]() {
      auto it = colony.installations.find("shipyard");
      if (it == colony.installations.end()) return 0;
      return std::max(0, it->second);
    }();
    out.shipyard_tons_per_day_start = shipyard_def->build_rate_tons_per_day * static_cast<double>(yards) *
                                      std::max(0.0, my_mult.shipyard);
  }

  // Copy body deposits for mining simulation.
  std::unordered_map<std::string, double> deposits;
  if (const Body* b = find_ptr(sim.state().bodies, colony.body_id)) {
    deposits = b->mineral_deposits;
  }

  const int max_days = std::max(0, opt.max_days);
  const int max_events = std::max(0, opt.max_events);

  auto auto_targets_need_work = [&]() -> bool {
    if (!opt.include_construction) return false;
    if (!opt.include_auto_construction_targets) return false;
    if (colony.installation_targets.empty()) return false;

    // Compute pending manual quantities by installation id.
    std::unordered_map<std::string, int> manual_pending;
    manual_pending.reserve(colony.construction_queue.size() * 2);
    for (const auto& ord : colony.construction_queue) {
      if (ord.installation_id.empty()) continue;
      if (ord.auto_queued) continue;
      const int qty = std::max(0, ord.quantity_remaining);
      if (qty <= 0) continue;
      manual_pending[ord.installation_id] += qty;
    }

    for (const auto& [inst_id, target_raw] : colony.installation_targets) {
      if (inst_id.empty()) continue;
      const int target = std::max(0, target_raw);
      if (target <= 0) continue;

      const int have = [&]() -> int {
        auto it = colony.installations.find(inst_id);
        return (it == colony.installations.end()) ? 0 : std::max(0, it->second);
      }();

      const int man = manual_pending[inst_id];
      if (have + man < target) return true;
    }

    return false;
  };

  auto first_unbuildable_auto_target = [&]() -> std::string {
    if (!opt.include_construction) return {};
    if (!opt.include_auto_construction_targets) return {};
    if (colony.installation_targets.empty()) return {};

    // Compute pending manual quantities by installation id.
    std::unordered_map<std::string, int> manual_pending;
    manual_pending.reserve(colony.construction_queue.size() * 2);
    for (const auto& ord : colony.construction_queue) {
      if (ord.installation_id.empty()) continue;
      if (ord.auto_queued) continue;
      const int qty = std::max(0, ord.quantity_remaining);
      if (qty <= 0) continue;
      manual_pending[ord.installation_id] += qty;
    }

    for (const auto& [inst_id, target_raw] : colony.installation_targets) {
      if (inst_id.empty()) continue;
      const int target = std::max(0, target_raw);
      if (target <= 0) continue;

      const int have = [&]() -> int {
        auto it = colony.installations.find(inst_id);
        return (it == colony.installations.end()) ? 0 : std::max(0, it->second);
      }();

      const int man = manual_pending[inst_id];
      if (have + man >= target) continue;

      if (!sim.is_installation_buildable_for_faction(colony.faction_id, inst_id)) {
        return inst_id;
      }
    }

    return {};
  };

  auto have_work = [&]() {
    const bool has_shipyard = opt.include_shipyard && !colony.shipyard_queue.empty();
    const bool has_construction = opt.include_construction && !colony.construction_queue.empty();
    return has_shipyard || has_construction || auto_targets_need_work();
  };

  // If there's nothing to forecast, return OK with empty events.
  if (!have_work()) {
    out.ok = true;
    out.minerals_end = colony.minerals;
    return out;
  }

  // If the only prospective work is from auto-targets, but the target requires
  // an unbuildable installation, surface that as a stall reason immediately.
  if (opt.include_construction && opt.include_auto_construction_targets && colony.construction_queue.empty()) {
    if (const std::string blocked = first_unbuildable_auto_target(); !blocked.empty()) {
      out.ok = true;
      out.stalled = true;
      out.stall_reason = "Installation target requires a locked/unbuildable installation: " + blocked;
      out.minerals_end = colony.minerals;
      return out;
    }
  }

  // Simulate day-by-day.
  for (int day = 1; day <= max_days; ++day) {
    if (max_events > 0 && static_cast<int>(out.events.size()) >= max_events) {
      out.ok = true;
      out.truncated = true;
      out.truncated_reason = "Reached max_events";
      out.minerals_end = colony.minerals;
      return out;
    }

    bool any_change = false;
    bool any_progress = false;

    // 1) tick_colonies (mining + industry) -- simplified to mineral flows.
    if (colony.body_id != kInvalidId) {
      any_change = simulate_mining_day(sim, fac_mult, colony_id, colony, deposits, 1.0) || any_change;
    }
    any_change = simulate_industry_day(sim.content(), colony, std::max(0.0, my_mult.industry), 1.0) || any_change;

    // 2) tick_shipyards
    if (opt.include_shipyard && shipyard_def) {
      bool hard_block = false;
      std::string hard_reason;
      const bool prog = simulate_shipyard_day(sim, colony, my_mult, *shipyard_def, day, out.events, hard_block, hard_reason);
      any_progress = any_progress || prog;
      if (hard_block) {
        out.ok = true;
        out.stalled = true;
        out.stall_reason = hard_reason;
        out.minerals_end = colony.minerals;
        return out;
      }
    }

    // 3) tick_construction
    if (opt.include_construction) {
      if (!opt.include_auto_construction_targets) {
        // Temporarily disable targets for this day.
        const auto saved_targets = colony.installation_targets;
        colony.installation_targets.clear();
        const bool prog = simulate_construction_day(sim, colony, day, out.events);
        colony.installation_targets = saved_targets;
        any_progress = any_progress || prog;
      } else {
        const bool prog = simulate_construction_day(sim, colony, day, out.events);
        any_progress = any_progress || prog;
      }
    }

    // Completion: nothing left and no targets that would add work.
    if (!have_work()) {
      out.ok = true;
      out.minerals_end = colony.minerals;
      return out;
    }

    // Stall detection: no mineral changes and no queue progress.
    if (!any_change && !any_progress) {
      out.ok = true;
      out.stalled = true;
      out.stall_reason = "No progress possible under local production assumptions";
      out.minerals_end = colony.minerals;
      return out;
    }
  }

  out.ok = true;
  out.truncated = true;
  out.truncated_reason = "Reached max_days";
  out.minerals_end = colony.minerals;
  return out;
}

} // namespace nebula4x
