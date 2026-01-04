#include "nebula4x/core/fuel_planner.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <unordered_set>

#include "nebula4x/core/simulation.h"

namespace nebula4x {
namespace {

constexpr double kEps = 1e-9;

template <typename Map>
std::vector<typename Map::key_type> sorted_keys(const Map& m) {
  std::vector<typename Map::key_type> keys;
  keys.reserve(m.size());
  for (const auto& kv : m) keys.push_back(kv.first);
  std::sort(keys.begin(), keys.end());
  return keys;
}

bool orders_empty(const GameState& st, Id ship_id) {
  auto it = st.ship_orders.find(ship_id);
  if (it == st.ship_orders.end()) return true;
  const ShipOrders& so = it->second;
  if (!so.queue.empty()) return false;
  // A ship with repeat enabled and remaining refills is not considered idle:
  // its queue will be refilled during tick_ships().
  if (so.repeat && !so.repeat_template.empty() && so.repeat_count_remaining != 0) return false;
  return true;
}

double clamp01(double x) {
  return std::clamp(x, 0.0, 1.0);
}

struct TargetInfo {
  Id ship_id{kInvalidId};
  Id system_id{kInvalidId};
  Vec2 pos{0.0, 0.0};
  double cap{0.0};
  double fuel{0.0};
  double frac{0.0};
  double need{0.0};
};

struct TankerInfo {
  Id ship_id{kInvalidId};
  Id system_id{kInvalidId};
  Vec2 pos{0.0, 0.0};
  double speed_km_s{0.0};
  double cap{0.0};
  double fuel{0.0};
  double reserve{0.0};
  double available{0.0};
};

}  // namespace

FuelPlannerResult compute_fuel_plan(const Simulation& sim, Id faction_id, const FuelPlannerOptions& opt) {
  FuelPlannerResult out;
  out.ok = false;

  const GameState& st = sim.state();
  if (!find_ptr(st.factions, faction_id)) {
    out.message = "Invalid faction_id.";
    return out;
  }

  const double request_threshold = clamp01(sim.cfg().auto_tanker_request_threshold_fraction);
  const double fill_target = clamp01(sim.cfg().auto_tanker_fill_target_fraction);
  const double min_transfer = std::max(0.0, sim.cfg().auto_tanker_min_transfer_tons);

  // Ships already targeted by an existing TransferFuelToShip order.
  std::unordered_set<Id> reserved_targets;
  for (Id sid : sorted_keys(st.ship_orders)) {
    const auto it = st.ship_orders.find(sid);
    if (it == st.ship_orders.end()) continue;
    for (const auto& ord : it->second.queue) {
      if (const auto* tf = std::get_if<TransferFuelToShip>(&ord)) {
        reserved_targets.insert(tf->target_ship_id);
      }
    }
  }

  std::vector<TargetInfo> targets;
  std::vector<TankerInfo> tankers;

  const auto ship_ids = sorted_keys(st.ships);

  bool tankers_truncated = false;
  bool targets_truncated = false;

  for (Id sid : ship_ids) {
    const Ship* ship = find_ptr(st.ships, sid);
    if (!ship) continue;
    if (ship->faction_id != faction_id) continue;
    if (ship->system_id == kInvalidId) continue;

    if (opt.restrict_to_discovered && !sim.is_system_discovered_by_faction(faction_id, ship->system_id)) {
      continue;
    }

    const ShipDesign* d = sim.find_design(ship->design_id);
    if (!d) continue;
    const double cap = std::max(0.0, d->fuel_capacity_tons);
    if (cap <= kEps) continue;

    double fuel = ship->fuel_tons;
    if (fuel < 0.0) fuel = cap;
    fuel = std::clamp(fuel, 0.0, cap);
    const double frac = (cap > kEps) ? (fuel / cap) : 0.0;

    // --- Candidate targets (ships needing fuel) ---
    {
      bool eligible = true;

      if (reserved_targets.contains(sid)) eligible = false;
      if (opt.require_idle && !orders_empty(st, sid)) eligible = false;
      if (opt.exclude_fleet_ships && sim.fleet_for_ship(sid) != kInvalidId) eligible = false;
      if (opt.exclude_ships_with_auto_refuel && ship->auto_refuel) eligible = false;
      if (frac + kEps >= request_threshold) eligible = false;

      const double desired = cap * fill_target;
      const double need = desired - fuel;
      if (need <= min_transfer + kEps) eligible = false;

      if (eligible) {
        TargetInfo t;
        t.ship_id = sid;
        t.system_id = ship->system_id;
        t.pos = ship->position_mkm;
        t.cap = cap;
        t.fuel = fuel;
        t.frac = frac;
        t.need = std::max(0.0, need);
        targets.push_back(std::move(t));
      }
    }

    // --- Candidate tankers ---
    {
      if (tankers_truncated) continue;

      bool eligible = true;
      if (opt.require_auto_tanker_flag && !ship->auto_tanker) eligible = false;
      if (opt.require_idle && !orders_empty(st, sid)) eligible = false;
      if (opt.exclude_fleet_ships && sim.fleet_for_ship(sid) != kInvalidId) eligible = false;
      if (ship->speed_km_s <= 0.0) eligible = false;

      // Avoid fighting other automation.
      if (ship->auto_explore || ship->auto_freight || ship->auto_salvage || ship->auto_colonize) eligible = false;

      if (eligible) {
        const double reserve_frac = clamp01(ship->auto_tanker_reserve_fraction);
        const double reserve = cap * reserve_frac;
        const double available = fuel - reserve;
        if (available > min_transfer + kEps) {
          TankerInfo t;
          t.ship_id = sid;
          t.system_id = ship->system_id;
          t.pos = ship->position_mkm;
          t.speed_km_s = ship->speed_km_s;
          t.cap = cap;
          t.fuel = fuel;
          t.reserve = reserve;
          t.available = std::max(0.0, available);
          tankers.push_back(std::move(t));
          if (static_cast<int>(tankers.size()) >= std::max(1, opt.max_tankers)) {
            if (sid != ship_ids.back()) tankers_truncated = true;
          }
        }
      }
    }
  }

  const int max_targets = std::max(1, opt.max_targets);
  if (static_cast<int>(targets.size()) > max_targets) {
    // Keep the most urgent targets (lowest fuel fraction). Tie-break by id for determinism.
    std::sort(targets.begin(), targets.end(), [](const TargetInfo& a, const TargetInfo& b) {
      if (std::abs(a.frac - b.frac) > 1e-9) return a.frac < b.frac;
      return a.ship_id < b.ship_id;
    });
    targets.resize(static_cast<size_t>(max_targets));
    targets_truncated = true;
  }

  // Deterministic ordering.
  std::sort(tankers.begin(), tankers.end(), [](const TankerInfo& a, const TankerInfo& b) {
    return a.ship_id < b.ship_id;
  });

  out.ok = true;

  if (targets.empty()) {
    out.message = "No ships need refueling.";
    return out;
  }
  if (tankers.empty()) {
    out.message = "No eligible tankers.";
    return out;
  }

  const int max_legs = std::max(1, opt.max_legs_per_tanker);

  std::vector<bool> served(targets.size(), false);
  int remaining = static_cast<int>(targets.size());

  int total_legs_planned = 0;

  for (const TankerInfo& tanker : tankers) {
    if (remaining <= 0) break;

    FuelAssignment asg;
    asg.tanker_ship_id = tanker.ship_id;
    asg.restrict_to_discovered = opt.restrict_to_discovered;
    asg.tanker_fuel_capacity_tons = tanker.cap;
    asg.tanker_fuel_before_tons = tanker.fuel;
    asg.tanker_fuel_reserved_tons = tanker.reserve;
    asg.tanker_fuel_available_tons = tanker.available;

    Id cur_sys = tanker.system_id;
    Vec2 cur_pos = tanker.pos;
    double available = tanker.available;
    double eta_total = 0.0;

    while (available > min_transfer + kEps && static_cast<int>(asg.legs.size()) < max_legs && remaining > 0) {
      int best_idx = -1;
      double best_frac = std::numeric_limits<double>::infinity();
      double best_eta = std::numeric_limits<double>::infinity();
      Id best_id = kInvalidId;

      for (int i = 0; i < static_cast<int>(targets.size()); ++i) {
        if (served[i]) continue;
        const TargetInfo& tgt = targets[i];
        if (tgt.ship_id == tanker.ship_id) continue;
        if (tgt.system_id == kInvalidId) continue;

        const auto plan = sim.plan_jump_route_from_pos(cur_sys, cur_pos, faction_id, tanker.speed_km_s,
                                                      tgt.system_id, opt.restrict_to_discovered, tgt.pos);
        if (!plan) continue;
        const double eta = plan->total_eta_days;

        // Primary: lowest fuel fraction (most urgent).
        // Secondary: shortest ETA.
        // Tertiary: stable id tie-break.
        bool better = false;
        if (tgt.frac + kEps < best_frac) {
          better = true;
        } else if (std::abs(tgt.frac - best_frac) <= 1e-6) {
          if (eta + 1e-6 < best_eta) {
            better = true;
          } else if (std::abs(eta - best_eta) <= 1e-6) {
            if (best_id == kInvalidId || tgt.ship_id < best_id) better = true;
          }
        }

        if (better) {
          best_idx = i;
          best_frac = tgt.frac;
          best_eta = eta;
          best_id = tgt.ship_id;
        }
      }

      if (best_idx < 0) break;

      const TargetInfo& tgt = targets[best_idx];
      const double give = std::min(available, tgt.need);
      if (!(give > min_transfer + kEps)) break;

      FuelTransferLeg leg;
      leg.target_ship_id = tgt.ship_id;
      leg.tons = give;
      leg.eta_days = best_eta;
      leg.target_fuel_frac_before = clamp01(tgt.frac);
      leg.target_fuel_frac_after = clamp01((tgt.fuel + give) / std::max(kEps, tgt.cap));
      asg.legs.push_back(std::move(leg));

      asg.fuel_transfer_total_tons += give;
      eta_total += best_eta;

      available -= give;
      served[best_idx] = true;
      --remaining;
      ++total_legs_planned;

      // Next leg starts from the target ship's current position.
      cur_sys = tgt.system_id;
      cur_pos = tgt.pos;
    }

    asg.eta_total_days = eta_total;

    if (!asg.legs.empty()) {
      out.assignments.push_back(std::move(asg));
    }
  }

  std::ostringstream ss;
  if (out.assignments.empty()) {
    ss << "No viable tanker transfers found.";
  } else {
    ss << "Planned " << total_legs_planned << " transfer" << (total_legs_planned == 1 ? "" : "s")
       << " across " << out.assignments.size() << " tanker" << (out.assignments.size() == 1 ? "" : "s")
       << " (" << targets.size() << " ship" << (targets.size() == 1 ? "" : "s")
       << " below threshold).";
  }
  if (tankers_truncated) {
    out.truncated = true;
    ss << " Candidate tankers truncated by max_tankers.";
  }
  if (targets_truncated) {
    out.truncated = true;
    ss << " Candidate targets truncated by max_targets.";
  }
  out.message = ss.str();

  return out;
}

bool apply_fuel_assignment(Simulation& sim, const FuelAssignment& asg, bool clear_existing_orders) {
  if (asg.tanker_ship_id == kInvalidId) return false;
  if (clear_existing_orders) {
    if (!sim.clear_orders(asg.tanker_ship_id)) return false;
  }
  for (const auto& leg : asg.legs) {
    if (leg.target_ship_id == kInvalidId) return false;
    if (leg.target_ship_id == asg.tanker_ship_id) return false;
    if (leg.tons < 0.0) return false;
    if (!sim.issue_transfer_fuel_to_ship(asg.tanker_ship_id, leg.target_ship_id, leg.tons,
                                         asg.restrict_to_discovered)) {
      return false;
    }
  }
  return true;
}

bool apply_fuel_plan(Simulation& sim, const FuelPlannerResult& plan, bool clear_existing_orders) {
  if (!plan.ok) return false;
  for (const auto& asg : plan.assignments) {
    if (!apply_fuel_assignment(sim, asg, clear_existing_orders)) return false;
  }
  return true;
}

}  // namespace nebula4x
