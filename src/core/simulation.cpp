#include "nebula4x/core/simulation.h"

#include "simulation_internal.h"

#include "simulation_nav_helpers.h"
#include "simulation_sensors.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <limits>
#include <optional>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "nebula4x/core/scenario.h"
#include "nebula4x/core/ai_economy.h"
#include "nebula4x/util/log.h"
#include "nebula4x/util/spatial_index.h"

namespace nebula4x {
namespace {
using sim_internal::kTwoPi;
using sim_internal::ascii_to_lower;
using sim_internal::is_mining_installation;
using sim_internal::mkm_per_day_from_speed;
using sim_internal::push_unique;
using sim_internal::vec_contains;
using sim_internal::sorted_keys;
using sim_internal::faction_has_tech;
using sim_internal::FactionEconomyMultipliers;
using sim_internal::compute_faction_economy_multipliers;
using sim_internal::compute_power_allocation;

using sim_nav::PredictedNavState;
using sim_nav::predicted_nav_state_after_queued_jumps;

using sim_sensors::SensorSource;
using sim_sensors::gather_sensor_sources;
using sim_sensors::any_source_detects;

struct JumpRouteNode {
  Id system_id{kInvalidId};
  Id entry_jump_id{kInvalidId}; // the jump point we are currently "at" in this system
};

bool operator==(const JumpRouteNode& a, const JumpRouteNode& b) {
  return a.system_id == b.system_id && a.entry_jump_id == b.entry_jump_id;
}

struct JumpRouteNodeHash {
  std::size_t operator()(const JumpRouteNode& n) const noexcept {
    std::size_t h1 = std::hash<Id>{}(n.system_id);
    std::size_t h2 = std::hash<Id>{}(n.entry_jump_id);
    // A simple hash combine.
    return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
  }
};

struct JumpRouteDist {
  double cost_mkm{0.0};
  int hops{0};
};

bool dist_better(const JumpRouteDist& a, const JumpRouteDist& b) {
  constexpr double kEps = 1e-9;
  if (a.cost_mkm + kEps < b.cost_mkm) return true;
  if (std::fabs(a.cost_mkm - b.cost_mkm) <= kEps && a.hops < b.hops) return true;
  return false;
}

struct JumpRoutePQItem {
  JumpRouteDist d;
  JumpRouteNode node;
};

struct JumpRoutePQComp {
  bool operator()(const JumpRoutePQItem& a, const JumpRoutePQItem& b) const {
    // priority_queue is max-heap; return true if a should come after b.
    if (a.d.cost_mkm != b.d.cost_mkm) return a.d.cost_mkm > b.d.cost_mkm;
    if (a.d.hops != b.d.hops) return a.d.hops > b.d.hops;
    if (a.node.system_id != b.node.system_id) return a.node.system_id > b.node.system_id;
    return a.node.entry_jump_id > b.node.entry_jump_id;
  }
};
static std::uint64_t canonical_double_bits(double v) {
  std::uint64_t u = 0;
  static_assert(sizeof(u) == sizeof(v));
  std::memcpy(&u, &v, sizeof(u));
  // Normalize -0.0 to +0.0 for stable hashing.
  if (u == 0x8000000000000000ULL) u = 0ULL;
  return u;
}

static void update_jump_route_eta(JumpRoutePlan& plan, double speed_km_s, double seconds_per_day) {
  // Keep totals consistent even if callers only set distance_mkm/final_leg_mkm.
  plan.total_distance_mkm = plan.distance_mkm + plan.final_leg_mkm;

  const double mkm_per_day = mkm_per_day_from_speed(speed_km_s, seconds_per_day);
  if (mkm_per_day <= 0.0) {
    plan.eta_days = std::numeric_limits<double>::infinity();
    plan.total_eta_days = std::numeric_limits<double>::infinity();
  } else {
    plan.eta_days = plan.distance_mkm / mkm_per_day;
    plan.total_eta_days = plan.total_distance_mkm / mkm_per_day;
  }
}



std::optional<JumpRoutePlan> compute_jump_route_plan(const Simulation& sim, Id start_system_id,
                                                    Vec2 start_pos_mkm, Id faction_id,
                                                    double speed_km_s, Id target_system_id,
                                                    bool restrict_to_discovered,
                                                    std::optional<Vec2> goal_pos_mkm) {
  const auto& s = sim.state();

  if (!find_ptr(s.systems, start_system_id)) return std::nullopt;
  if (!find_ptr(s.systems, target_system_id)) return std::nullopt;

  const bool has_goal = goal_pos_mkm.has_value();

  // If we're restricted to a faction's discovered map (fog-of-war), prebuild a fast membership set.
  const Faction* fac = nullptr;
  std::unordered_set<Id> discovered;
  std::unordered_set<Id> surveyed_jumps;
  if (restrict_to_discovered) {
    fac = find_ptr(s.factions, faction_id);
    if (fac) {
      discovered.reserve(fac->discovered_systems.size() * 2 + 8);
      for (Id sid : fac->discovered_systems) discovered.insert(sid);
      // Ensure the starting system is always considered allowed.
      discovered.insert(start_system_id);

      surveyed_jumps.reserve(fac->surveyed_jump_points.size() * 2 + 8);
      for (Id jid : fac->surveyed_jump_points) surveyed_jumps.insert(jid);
    }
  }

  auto allow_system = [&](Id sys_id) {
    if (!restrict_to_discovered) return true;
    // Backward-compat: if the faction doesn't exist, don't block routing.
    if (!fac) return true;
    return discovered.contains(sys_id);
  };

  auto allow_jump = [&](Id jump_id) {
    if (!restrict_to_discovered) return true;
    // Backward-compat: if the faction doesn't exist, don't block routing.
    if (!fac) return true;
    return surveyed_jumps.contains(jump_id);
  };

  if (restrict_to_discovered && !allow_system(target_system_id)) return std::nullopt;

  // Same-system: no jump routing needed, but if a goal position is provided
  // we still fill in the "final leg" fields for ETA estimation.
  if (start_system_id == target_system_id) {
    JumpRoutePlan plan;
    plan.systems = {start_system_id};
    plan.distance_mkm = 0.0;
    plan.has_goal_pos = has_goal;
    plan.arrival_pos_mkm = start_pos_mkm;
    if (has_goal) {
      plan.goal_pos_mkm = *goal_pos_mkm;
      plan.final_leg_mkm = (plan.goal_pos_mkm - plan.arrival_pos_mkm).length();
    }
    update_jump_route_eta(plan, speed_km_s, sim.cfg().seconds_per_day);
    return plan;
  }

  // Cache per-system outgoing jump lists (sorted/unique) for the duration of this solve.
  // This avoids re-sorting the same system's jump list many times during Dijkstra.
  std::unordered_map<Id, std::vector<Id>> outgoing_cache;
  outgoing_cache.reserve(32);
  auto outgoing_jumps = [&](Id system_id) -> const std::vector<Id>& {
    auto it = outgoing_cache.find(system_id);
    if (it != outgoing_cache.end()) return it->second;

    std::vector<Id> out;
    if (const auto* sys = find_ptr(s.systems, system_id)) {
      out = sys->jump_points;
      std::sort(out.begin(), out.end());
      out.erase(std::unique(out.begin(), out.end()), out.end());
    }

    auto [ins, _] = outgoing_cache.emplace(system_id, std::move(out));
    return ins->second;
  };

  std::priority_queue<JumpRoutePQItem, std::vector<JumpRoutePQItem>, JumpRoutePQComp> pq;
  std::unordered_map<JumpRouteNode, JumpRouteDist, JumpRouteNodeHash> dist;
  std::unordered_map<JumpRouteNode, JumpRouteNode, JumpRouteNodeHash> prev;
  std::unordered_map<JumpRouteNode, Id, JumpRouteNodeHash> prev_jump;

  const JumpRouteNode start{start_system_id, kInvalidId};
  dist[start] = JumpRouteDist{0.0, 0};
  prev[start] = JumpRouteNode{kInvalidId, kInvalidId};
  pq.push(JumpRoutePQItem{dist[start], start});

  JumpRouteNode best_goal{kInvalidId, kInvalidId};
  JumpRouteDist best_goal_dist{};
  double best_goal_total_cost = std::numeric_limits<double>::infinity();

  constexpr double kEps = 1e-9;

  auto goal_better = [&](double total_cost, const JumpRouteDist& cand_dist, const JumpRouteNode& cand_node) {
    if (total_cost + kEps < best_goal_total_cost) return true;
    if (std::fabs(total_cost - best_goal_total_cost) <= kEps) {
      // Tie-breaker: fewer hops, then lower jump-network cost, then deterministic ids.
      if (cand_dist.hops < best_goal_dist.hops) return true;
      if (cand_dist.hops == best_goal_dist.hops) {
        if (cand_dist.cost_mkm + kEps < best_goal_dist.cost_mkm) return true;
        if (std::fabs(cand_dist.cost_mkm - best_goal_dist.cost_mkm) <= kEps) {
          if (cand_node.entry_jump_id < best_goal.entry_jump_id) return true;
        }
      }
    }
    return false;
  };

  while (!pq.empty()) {
    const JumpRoutePQItem cur = pq.top();
    pq.pop();

    const auto it_best = dist.find(cur.node);
    if (it_best == dist.end()) continue;
    if (dist_better(it_best->second, cur.d)) continue; // stale

    // If we already have a complete goal candidate, we can stop once the best
    // remaining cost-to-node cannot possibly beat it (terminal cost is >= 0).
    if (has_goal && std::isfinite(best_goal_total_cost)) {
      if (cur.d.cost_mkm > best_goal_total_cost + kEps) break;
    }

    const auto* sys = find_ptr(s.systems, cur.node.system_id);
    if (!sys) continue;

    Vec2 cur_pos = start_pos_mkm;
    if (cur.node.entry_jump_id != kInvalidId) {
      const auto* entry = find_ptr(s.jump_points, cur.node.entry_jump_id);
      if (!entry || entry->system_id != cur.node.system_id) continue;
      cur_pos = entry->position_mkm;
    }

    if (cur.node.system_id == target_system_id) {
      const double terminal = has_goal ? (cur_pos - *goal_pos_mkm).length() : 0.0;
      const double total_cost = cur.d.cost_mkm + terminal;

      if (best_goal.system_id == kInvalidId || goal_better(total_cost, cur.d, cur.node)) {
        best_goal = cur.node;
        best_goal_dist = cur.d;
        best_goal_total_cost = total_cost;
      }

      if (!has_goal) {
        // Old behavior: stop at the cheapest arrival into the destination system.
        break;
      }
      // In goal-aware mode we continue exploring: it may be cheaper overall to reach a different
      // entry jump (possibly via cycles) once the terminal distance is considered.
    }

    const std::vector<Id>& outgoing = outgoing_jumps(cur.node.system_id);
    for (Id jid : outgoing) {
      if (!allow_jump(jid)) continue;
      const auto* jp = find_ptr(s.jump_points, jid);
      if (!jp) continue;
      if (jp->system_id != cur.node.system_id) continue;
      if (jp->linked_jump_id == kInvalidId) continue;

      const auto* dest_jp = find_ptr(s.jump_points, jp->linked_jump_id);
      if (!dest_jp) continue;
      const Id next_sys = dest_jp->system_id;
      if (next_sys == kInvalidId) continue;
      if (!find_ptr(s.systems, next_sys)) continue;
      if (restrict_to_discovered && !allow_system(next_sys)) continue;

      const Vec2 jp_pos = jp->position_mkm;
      const double leg = (jp_pos - cur_pos).length();

      const JumpRouteNode nxt{next_sys, dest_jp->id};
      const JumpRouteDist cand{cur.d.cost_mkm + leg, cur.d.hops + 1};

      auto it = dist.find(nxt);
      if (it == dist.end() || dist_better(cand, it->second)) {
        dist[nxt] = cand;
        prev[nxt] = cur.node;
        prev_jump[nxt] = jid;
        pq.push(JumpRoutePQItem{cand, nxt});
      }
    }
  }

  if (best_goal.system_id == kInvalidId) return std::nullopt;

  // Reconstruct jump ids.
  std::vector<Id> jump_ids;
  JumpRouteNode cur = best_goal;
  while (!(cur == start)) {
    auto itp = prev.find(cur);
    auto itj = prev_jump.find(cur);
    if (itp == prev.end() || itj == prev_jump.end()) return std::nullopt;
    jump_ids.push_back(itj->second);
    cur = itp->second;
  }
  std::reverse(jump_ids.begin(), jump_ids.end());

  // Build system list from jump ids.
  std::vector<Id> systems;
  systems.reserve(jump_ids.size() + 1);
  systems.push_back(start_system_id);
  Id sys_id = start_system_id;
  for (Id jid : jump_ids) {
    const auto* jp = find_ptr(s.jump_points, jid);
    if (!jp || jp->system_id != sys_id || jp->linked_jump_id == kInvalidId) return std::nullopt;
    const auto* dest = find_ptr(s.jump_points, jp->linked_jump_id);
    if (!dest) return std::nullopt;
    sys_id = dest->system_id;
    systems.push_back(sys_id);
  }

  JumpRoutePlan plan;
  plan.systems = std::move(systems);
  plan.jump_ids = std::move(jump_ids);

  // Best-known distance is stored in dist for the goal node.
  const auto it_goal = dist.find(best_goal);
  if (it_goal != dist.end()) plan.distance_mkm = it_goal->second.cost_mkm;

  // Arrival position: the entry jump in the destination system.
  plan.arrival_pos_mkm = start_pos_mkm;
  if (best_goal.entry_jump_id != kInvalidId) {
    if (const auto* entry = find_ptr(s.jump_points, best_goal.entry_jump_id)) {
      if (entry->system_id == target_system_id) {
        plan.arrival_pos_mkm = entry->position_mkm;
      }
    }
  }

  plan.has_goal_pos = has_goal;
  if (has_goal) {
    plan.goal_pos_mkm = *goal_pos_mkm;
    plan.final_leg_mkm = (plan.goal_pos_mkm - plan.arrival_pos_mkm).length();
  }

  update_jump_route_eta(plan, speed_km_s, sim.cfg().seconds_per_day);
  return plan;
}

} // namespace

Simulation::Simulation(ContentDB content, SimConfig cfg) : content_(std::move(content)), cfg_(cfg) {
  new_game();
}

const ShipDesign* Simulation::find_design(const std::string& design_id) const {
  if (auto it = state_.custom_designs.find(design_id); it != state_.custom_designs.end()) return &it->second;
  if (auto it = content_.designs.find(design_id); it != content_.designs.end()) return &it->second;
  return nullptr;
}


bool Simulation::is_ship_docked_at_colony(Id ship_id, Id colony_id) const {
  const auto* ship = find_ptr(state_.ships, ship_id);
  const auto* colony = find_ptr(state_.colonies, colony_id);
  if (!ship || !colony) return false;

  const auto* body = find_ptr(state_.bodies, colony->body_id);
  if (!body) return false;
  if (body->system_id != ship->system_id) return false;

  const double dock_range = std::max(0.0, cfg_.docking_range_mkm);
  const double dist = (ship->position_mkm - body->position_mkm).length();
  return dist <= dock_range + 1e-9;
}

bool Simulation::is_design_buildable_for_faction(Id faction_id, const std::string& design_id) const {
  const auto* d = find_design(design_id);
  if (!d) return false;

  const auto* fac = find_ptr(state_.factions, faction_id);
  if (!fac) return true;

  for (const auto& cid : d->components) {
    if (!vec_contains(fac->unlocked_components, cid)) return false;
  }
  return true;
}

bool Simulation::is_installation_buildable_for_faction(Id faction_id, const std::string& installation_id) const {
  if (content_.installations.find(installation_id) == content_.installations.end()) return false;

  const auto* fac = find_ptr(state_.factions, faction_id);
  if (!fac) return true;

  return vec_contains(fac->unlocked_installations, installation_id);
}

double Simulation::construction_points_per_day(const Colony& colony) const {
  double total = std::max(0.0, colony.population_millions * 0.01);

  for (const auto& [inst_id, count] : colony.installations) {
    if (count <= 0) continue;
    const auto it = content_.installations.find(inst_id);
    if (it == content_.installations.end()) continue;
    const double per_day = it->second.construction_points_per_day;
    if (per_day <= 0.0) continue;
    total += per_day * static_cast<double>(count);
  }

  // Tech-driven faction modifier.
  if (const auto* fac = find_ptr(state_.factions, colony.faction_id)) {
    const auto m = compute_faction_economy_multipliers(content_, *fac);
    total *= std::max(0.0, m.construction);
  }

  return std::max(0.0, total);
}

double Simulation::body_habitability(Id body_id) const {
  if (!cfg_.enable_habitability) return 1.0;

  const auto* b = find_ptr(state_.bodies, body_id);
  if (!b) return 1.0;

  // If terraforming is marked complete, treat as fully habitable.
  if (b->terraforming_complete) return 1.0;

  // Use terraforming targets as the "ideal" environment when present.
  // This makes partial terraforming gradually improve habitability.
  double ideal_t = cfg_.habitability_ideal_temp_k;
  double ideal_atm = cfg_.habitability_ideal_atm;
  if (b->terraforming_target_temp_k > 0.0) ideal_t = b->terraforming_target_temp_k;
  if (b->terraforming_target_atm > 0.0) ideal_atm = b->terraforming_target_atm;

  const double dt = std::fabs(b->surface_temp_k - ideal_t);
  const double da = std::fabs(b->atmosphere_atm - ideal_atm);

  auto factor_linear = [](double delta, double tol) {
    if (!(tol > 0.0)) return (delta <= 0.0) ? 1.0 : 0.0;
    const double x = 1.0 - (delta / tol);
    return std::clamp(x, 0.0, 1.0);
  };

  const double t_tol = std::max(1e-9, cfg_.habitability_temp_tolerance_k);
  const double a_tol = std::max(1e-9, cfg_.habitability_atm_tolerance);

  const double t_factor = factor_linear(dt, t_tol);
  const double a_factor = factor_linear(da, a_tol);

  return std::clamp(t_factor * a_factor, 0.0, 1.0);
}

double Simulation::habitation_capacity_millions(const Colony& colony) const {
  double total = 0.0;
  for (const auto& [inst_id, count] : colony.installations) {
    if (count <= 0) continue;
    const auto it = content_.installations.find(inst_id);
    if (it == content_.installations.end()) continue;
    const double cap = it->second.habitation_capacity_millions;
    if (cap <= 0.0) continue;
    total += cap * static_cast<double>(count);
  }
  return std::max(0.0, total);
}

double Simulation::required_habitation_capacity_millions(const Colony& colony) const {
  if (!cfg_.enable_habitability) return 0.0;
  const double pop = std::max(0.0, colony.population_millions);
  if (pop <= 0.0) return 0.0;

  const double hab = body_habitability(colony.body_id);
  if (hab >= 0.999) return 0.0;
  return pop * std::clamp(1.0 - hab, 0.0, 1.0);
}

std::vector<LogisticsNeed> Simulation::logistics_needs_for_faction(Id faction_id) const {
  std::vector<LogisticsNeed> out;
  if (faction_id == kInvalidId) return out;

  const double shipyard_rate_mult = [&]() {
    const auto* fac = find_ptr(state_.factions, faction_id);
    if (!fac) return 1.0;
    const auto m = compute_faction_economy_multipliers(content_, *fac);
    return std::max(0.0, m.shipyard);
  }();

  const InstallationDef* shipyard_def = nullptr;
  if (auto it = content_.installations.find("shipyard"); it != content_.installations.end()) {
    shipyard_def = &it->second;
  }

  const auto colony_ids = sorted_keys(state_.colonies);
  const auto ship_ids = sorted_keys(state_.ships);
  for (Id cid : colony_ids) {
    const Colony* colony = find_ptr(state_.colonies, cid);
    if (!colony) continue;
    if (colony->faction_id != faction_id) continue;

    // Shipyard needs: keep enough minerals to run the shipyard at full capacity for one day.
    if (shipyard_def) {
      int yards = 0;
      if (auto it = colony->installations.find(shipyard_def->id); it != colony->installations.end()) {
        yards = std::max(0, it->second);
      }

      if (yards > 0 && !colony->shipyard_queue.empty() && shipyard_def->build_rate_tons_per_day > 0.0 &&
          !shipyard_def->build_costs_per_ton.empty()) {
        const double capacity_tons =
            shipyard_def->build_rate_tons_per_day * static_cast<double>(yards) * shipyard_rate_mult;
        for (const auto& [mineral, cost_per_ton] : shipyard_def->build_costs_per_ton) {
          const double desired = std::max(0.0, cost_per_ton) * capacity_tons;
          if (desired <= 1e-9) continue;
          const double have = [&]() {
            if (auto it2 = colony->minerals.find(mineral); it2 != colony->minerals.end()) return it2->second;
            return 0.0;
          }();

          LogisticsNeed n;
          n.colony_id = cid;
          n.kind = LogisticsNeedKind::Shipyard;
          n.mineral = mineral;
          n.desired_tons = desired;
          n.have_tons = have;
          n.missing_tons = std::max(0.0, desired - have);
          out.push_back(std::move(n));
        }
      }
    }

    // Construction needs: installation build orders that have not yet paid minerals.
    for (const auto& ord : colony->construction_queue) {
      if (ord.quantity_remaining <= 0) continue;
      if (ord.minerals_paid) continue;

      auto it = content_.installations.find(ord.installation_id);
      if (it == content_.installations.end()) continue;
      const InstallationDef& def = it->second;

      for (const auto& [mineral, cost] : def.build_costs) {
        const double desired = std::max(0.0, cost);
        if (desired <= 1e-9) continue;
        const double have = [&]() {
          if (auto it2 = colony->minerals.find(mineral); it2 != colony->minerals.end()) return it2->second;
          return 0.0;
        }();
        const double missing = std::max(0.0, desired - have);
        if (missing <= 1e-9) continue;

        LogisticsNeed n;
        n.colony_id = cid;
        n.kind = LogisticsNeedKind::Construction;
        n.mineral = mineral;
        n.desired_tons = desired;
        n.have_tons = have;
        n.missing_tons = missing;
        n.context_id = def.id;
        out.push_back(std::move(n));
      }
    }

    // Troop training needs: keep enough minerals to train troops at full capacity
    // for one day.
    //
    // This covers both:
    // - explicit troop_training_queue, and
    // - implicit queue implied by garrison_target_strength (even if tick_ground_combat
    //   hasn't yet topped up troop_training_queue).
    //
    // The goal is QoL: with auto-freight enabled, colonies won't export the very
    // minerals required to keep training running, and will request imports when
    // short.
    {
      const bool has_costs = (cfg_.troop_training_duranium_per_strength > 1e-9) ||
                             (cfg_.troop_training_neutronium_per_strength > 1e-9);
      if (has_costs && cfg_.troop_strength_per_training_point > 1e-9) {
        const double points = std::max(0.0, troop_training_points_per_day(*colony));
        if (points > 1e-9) {
          const double strength_per_day = points * std::max(0.0, cfg_.troop_strength_per_training_point);
          if (strength_per_day > 1e-9) {
            // If a battle is ongoing, the battle record is authoritative.
            const double defender_strength = [&]() {
              if (auto itb = state_.ground_battles.find(cid); itb != state_.ground_battles.end()) {
                return std::max(0.0, itb->second.defender_strength);
              }
              return std::max(0.0, colony->ground_forces);
            }();

            const double target = std::max(0.0, colony->garrison_target_strength);
            const double required_queue_total = (target > 1e-9) ? std::max(0.0, target - defender_strength) : 0.0;

            const double planned_queue = std::max(0.0, std::max(colony->troop_training_queue, required_queue_total));
            const double strength_buffer = std::min(planned_queue, strength_per_day);

            auto add_need = [&](const char* mineral, double per_strength) {
              const double desired = strength_buffer * std::max(0.0, per_strength);
              if (desired <= 1e-9) return;
              const double have = [&]() {
                if (auto it2 = colony->minerals.find(mineral); it2 != colony->minerals.end()) return it2->second;
                return 0.0;
              }();

              LogisticsNeed n;
              n.colony_id = cid;
              n.kind = LogisticsNeedKind::TroopTraining;
              n.mineral = mineral;
              n.desired_tons = desired;
              n.have_tons = have;
              n.missing_tons = std::max(0.0, desired - have);
              out.push_back(std::move(n));
            };

            if (strength_buffer > 1e-9) {
              add_need("Duranium", cfg_.troop_training_duranium_per_strength);
              add_need("Neutronium", cfg_.troop_training_neutronium_per_strength);
            }
          }
        }
      }
    }

    // Industry input needs: keep a buffer of minerals required by daily-running non-mining industry.
    const double buffer_days = std::max(0.0, cfg_.auto_freight_industry_input_buffer_days);
    if (buffer_days > 1e-9) {
      std::unordered_map<std::string, double> per_day_inputs;
      for (const auto& [inst_id, count_raw] : colony->installations) {
        const int count = std::max(0, count_raw);
        if (count <= 0) continue;
        const auto it = content_.installations.find(inst_id);
        if (it == content_.installations.end()) continue;
        const InstallationDef& def = it->second;
        if (is_mining_installation(def)) continue;
        for (const auto& [mineral, per_day_raw] : def.consumes_per_day) {
          const double per_day = std::max(0.0, per_day_raw);
          if (per_day <= 1e-12) continue;
          per_day_inputs[mineral] += per_day * static_cast<double>(count);
        }
      }

      if (!per_day_inputs.empty()) {
        std::vector<std::string> minerals;
        minerals.reserve(per_day_inputs.size());
        for (const auto& [m, _] : per_day_inputs) minerals.push_back(m);
        std::sort(minerals.begin(), minerals.end());

        for (const std::string& mineral : minerals) {
          const double per_day = per_day_inputs[mineral];
          const double desired = per_day * buffer_days;
          if (desired <= 1e-9) continue;

          const double have = [&]() {
            if (auto it2 = colony->minerals.find(mineral); it2 != colony->minerals.end()) return it2->second;
            return 0.0;
          }();

          LogisticsNeed n;
          n.colony_id = cid;
          n.kind = LogisticsNeedKind::IndustryInput;
          n.mineral = mineral;
          n.desired_tons = desired;
          n.have_tons = have;
          n.missing_tons = std::max(0.0, desired - have);
          out.push_back(std::move(n));
        }
      }
    }

    // Stockpile target needs: user-defined desired mineral levels at the colony.
    //
    // These are a general-purpose logistics primitive used by auto-freight to
    // pre-stock forward bases or maintain buffers (e.g. Fuel at a staging colony).
    //
    // Targets act as both:
    // - a desired amount to import toward (missing_tons), and
    // - a soft export floor (reserve) in auto-freight.
    if (!colony->mineral_targets.empty()) {
      std::vector<std::string> target_keys;
      target_keys.reserve(colony->mineral_targets.size());
      for (const auto& [m, _] : colony->mineral_targets) target_keys.push_back(m);
      std::sort(target_keys.begin(), target_keys.end());

      for (const std::string& mineral : target_keys) {
        const double desired = std::max(0.0, colony->mineral_targets.at(mineral));
        if (desired <= 1e-9) continue;

        const double have = [&]() {
          if (auto it2 = colony->minerals.find(mineral); it2 != colony->minerals.end()) return it2->second;
          return 0.0;
        }();

        LogisticsNeed n;
        n.colony_id = cid;
        n.kind = LogisticsNeedKind::StockpileTarget;
        n.mineral = mineral;
        n.desired_tons = desired;
        n.have_tons = have;
        n.missing_tons = std::max(0.0, desired - have);
        out.push_back(std::move(n));
      }
    }

    // Fuel needs: enough Fuel at the colony to top up docked ships.
    //
    // Note: this is a "stockpile desired" need (like shipyards), not a per-day rate.
    const Body* body = find_ptr(state_.bodies, colony->body_id);
    if (body) {
      const double dock_range = std::max(0.0, cfg_.docking_range_mkm);

      double desired = 0.0;
      for (Id sid : ship_ids) {
        const Ship* ship = find_ptr(state_.ships, sid);
        if (!ship) continue;
        if (ship->faction_id != faction_id) continue;
        if (ship->system_id != body->system_id) continue;

        const double dist = (ship->position_mkm - body->position_mkm).length();
        if (dist > dock_range + 1e-9) continue;

        const ShipDesign* d = find_design(ship->design_id);
        if (!d) continue;
        const double cap = std::max(0.0, d->fuel_capacity_tons);
        const double have_ship = std::max(0.0, ship->fuel_tons);
        if (cap <= have_ship + 1e-9) continue;

        desired += (cap - have_ship);
      }

      if (desired > 1e-9) {
        const double have = [&]() {
          if (auto it2 = colony->minerals.find("Fuel"); it2 != colony->minerals.end()) return it2->second;
          return 0.0;
        }();

        LogisticsNeed n;
        n.colony_id = cid;
        n.kind = LogisticsNeedKind::Fuel;
        n.mineral = "Fuel";
        n.desired_tons = desired;
        n.have_tons = have;
        n.missing_tons = std::max(0.0, desired - have);
        out.push_back(std::move(n));
      }
    }
  }

  return out;
}

bool Simulation::is_system_discovered_by_faction(Id viewer_faction_id, Id system_id) const {
  const auto* fac = find_ptr(state_.factions, viewer_faction_id);
  if (!fac) return true;
  return std::find(fac->discovered_systems.begin(), fac->discovered_systems.end(), system_id) !=
         fac->discovered_systems.end();
}

bool Simulation::is_jump_point_surveyed_by_faction(Id viewer_faction_id, Id jump_point_id) const {
  if (jump_point_id == kInvalidId) return false;
  // When there is no "viewer" (e.g. omniscient mode / tools), treat all links as known.
  if (viewer_faction_id == kInvalidId) return true;
  const auto* fac = find_ptr(state_.factions, viewer_faction_id);
  if (!fac) return true;
  return std::find(fac->surveyed_jump_points.begin(), fac->surveyed_jump_points.end(), jump_point_id) !=
         fac->surveyed_jump_points.end();
}


void Simulation::ensure_jump_route_cache_current() const {
  const std::int64_t day = state_.date.days_since_epoch();
  if (!jump_route_cache_day_valid_ || jump_route_cache_day_ != day) {
    jump_route_cache_.clear();
    jump_route_cache_lru_.clear();
    jump_route_cache_day_ = day;
    jump_route_cache_day_valid_ = true;
  }
}

void Simulation::invalidate_jump_route_cache() const {
  jump_route_cache_.clear();
  jump_route_cache_lru_.clear();
  jump_route_cache_day_ = state_.date.days_since_epoch();
  jump_route_cache_day_valid_ = true;
}

void Simulation::touch_jump_route_cache_entry(JumpRouteCacheMap::iterator it) const {
  if (it == jump_route_cache_.end()) return;
  jump_route_cache_lru_.splice(jump_route_cache_lru_.begin(), jump_route_cache_lru_, it->second.lru_it);
  it->second.lru_it = jump_route_cache_lru_.begin();
}

std::optional<JumpRoutePlan> Simulation::plan_jump_route_cached(Id start_system_id, Vec2 start_pos_mkm, Id faction_id,
                                                               double speed_km_s, Id target_system_id,
                                                               bool restrict_to_discovered,
                                                               std::optional<Vec2> goal_pos_mkm) const {
  ensure_jump_route_cache_current();

  JumpRouteCacheKey key;
  key.start_system_id = start_system_id;
  key.start_pos_x_bits = canonical_double_bits(start_pos_mkm.x);
  key.start_pos_y_bits = canonical_double_bits(start_pos_mkm.y);
  key.faction_id = faction_id;
  key.target_system_id = target_system_id;
  key.restrict_to_discovered = restrict_to_discovered;
  key.has_goal_pos = goal_pos_mkm.has_value();
  if (key.has_goal_pos) {
    key.goal_pos_x_bits = canonical_double_bits(goal_pos_mkm->x);
    key.goal_pos_y_bits = canonical_double_bits(goal_pos_mkm->y);
  }

  auto it = jump_route_cache_.find(key);
  if (it != jump_route_cache_.end()) {
    ++jump_route_cache_hits_;
    touch_jump_route_cache_entry(it);

    JumpRoutePlan plan = it->second.plan;
    update_jump_route_eta(plan, speed_km_s, cfg_.seconds_per_day);
    return plan;
  }

  ++jump_route_cache_misses_;

  auto plan = compute_jump_route_plan(*this, start_system_id, start_pos_mkm, faction_id, speed_km_s,
                                      target_system_id, restrict_to_discovered, goal_pos_mkm);
  if (!plan) return std::nullopt;

  // Ensure eta is consistent with requested speed.
  update_jump_route_eta(*plan, speed_km_s, cfg_.seconds_per_day);

  // LRU insert; only cache successful plans to avoid stale negatives under fog-of-war expansion.
  if (jump_route_cache_.size() >= kJumpRouteCacheCapacity) {
    if (!jump_route_cache_lru_.empty()) {
      const JumpRouteCacheKey victim = jump_route_cache_lru_.back();
      jump_route_cache_.erase(victim);
      jump_route_cache_lru_.pop_back();
    } else {
      jump_route_cache_.clear();
    }
  }

  jump_route_cache_lru_.push_front(key);
  JumpRouteCacheEntry entry;
  entry.plan = *plan;
  entry.lru_it = jump_route_cache_lru_.begin();
  jump_route_cache_.emplace(key, std::move(entry));

  return plan;
}

std::optional<JumpRoutePlan> Simulation::plan_jump_route_for_ship(Id ship_id, Id target_system_id,
                                                                 bool restrict_to_discovered,
                                                                 bool include_queued_jumps) const {
  const auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return std::nullopt;

  const PredictedNavState nav = predicted_nav_state_after_queued_jumps(state_, ship_id, include_queued_jumps);
  if (nav.system_id == kInvalidId) return std::nullopt;

  return plan_jump_route_cached(nav.system_id, nav.position_mkm, ship->faction_id, ship->speed_km_s,
                               target_system_id, restrict_to_discovered);
}

std::optional<JumpRoutePlan> Simulation::plan_jump_route_for_ship_to_pos(Id ship_id, Id target_system_id,
                                                                        Vec2 goal_pos_mkm,
                                                                        bool restrict_to_discovered,
                                                                        bool include_queued_jumps) const {
  const auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return std::nullopt;

  const PredictedNavState nav = predicted_nav_state_after_queued_jumps(state_, ship_id, include_queued_jumps);
  if (nav.system_id == kInvalidId) return std::nullopt;

  return plan_jump_route_cached(nav.system_id, nav.position_mkm, ship->faction_id, ship->speed_km_s,
                               target_system_id, restrict_to_discovered, goal_pos_mkm);
}

std::optional<JumpRoutePlan> Simulation::plan_jump_route_for_fleet_to_pos(Id fleet_id, Id target_system_id,
                                                                         Vec2 goal_pos_mkm,
                                                                         bool restrict_to_discovered,
                                                                         bool include_queued_jumps) const {
  const auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return std::nullopt;
  if (fl->ship_ids.empty()) return std::nullopt;

  // Pick a leader for planning: prefer the explicit fleet leader, otherwise first valid ship.
  Id leader_id = fl->leader_ship_id;
  const Ship* leader = (leader_id != kInvalidId) ? find_ptr(state_.ships, leader_id) : nullptr;
  if (!leader) {
    leader_id = kInvalidId;
    for (Id sid : fl->ship_ids) {
      if (const auto* sh = find_ptr(state_.ships, sid)) {
        leader_id = sid;
        leader = sh;
        break;
      }
    }
  }
  if (!leader) return std::nullopt;

  const PredictedNavState nav = predicted_nav_state_after_queued_jumps(state_, leader_id, include_queued_jumps);
  if (nav.system_id == kInvalidId) return std::nullopt;

  // Conservative ETA: use the slowest ship speed.
  double slowest = std::numeric_limits<double>::infinity();
  for (Id sid : fl->ship_ids) {
    const auto* sh = find_ptr(state_.ships, sid);
    if (!sh) continue;
    slowest = std::min(slowest, sh->speed_km_s);
  }
  if (!std::isfinite(slowest)) slowest = leader->speed_km_s;

  return plan_jump_route_cached(nav.system_id, nav.position_mkm, fl->faction_id, slowest, target_system_id,
                               restrict_to_discovered, goal_pos_mkm);
}

std::optional<JumpRoutePlan> Simulation::plan_jump_route_for_fleet(Id fleet_id, Id target_system_id,
                                                                  bool restrict_to_discovered,
                                                                  bool include_queued_jumps) const {
  const auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return std::nullopt;
  if (fl->ship_ids.empty()) return std::nullopt;

  // Pick a leader for planning: prefer the explicit fleet leader, otherwise first valid ship.
  Id leader_id = fl->leader_ship_id;
  const Ship* leader = (leader_id != kInvalidId) ? find_ptr(state_.ships, leader_id) : nullptr;
  if (!leader) {
    leader_id = kInvalidId;
    for (Id sid : fl->ship_ids) {
      if (const auto* sh = find_ptr(state_.ships, sid)) {
        leader_id = sid;
        leader = sh;
        break;
      }
    }
  }
  if (!leader) return std::nullopt;

  const PredictedNavState nav = predicted_nav_state_after_queued_jumps(state_, leader_id, include_queued_jumps);
  if (nav.system_id == kInvalidId) return std::nullopt;

  // Conservative ETA: use the slowest ship speed.
  double slowest = std::numeric_limits<double>::infinity();
  for (Id sid : fl->ship_ids) {
    const auto* sh = find_ptr(state_.ships, sid);
    if (!sh) continue;
    slowest = std::min(slowest, sh->speed_km_s);
  }
  if (!std::isfinite(slowest)) slowest = leader->speed_km_s;

  return plan_jump_route_cached(nav.system_id, nav.position_mkm, fl->faction_id, slowest, target_system_id,
                               restrict_to_discovered);
}


DiplomacyStatus Simulation::diplomatic_status(Id from_faction_id, Id to_faction_id) const {
  if (from_faction_id == kInvalidId || to_faction_id == kInvalidId) return DiplomacyStatus::Hostile;
  if (from_faction_id == to_faction_id) return DiplomacyStatus::Friendly;

  const auto* from = find_ptr(state_.factions, from_faction_id);
  if (!from) return DiplomacyStatus::Hostile;

  auto it = from->relations.find(to_faction_id);
  if (it == from->relations.end()) return DiplomacyStatus::Hostile;
  return it->second;
}

bool Simulation::are_factions_hostile(Id from_faction_id, Id to_faction_id) const {
  return diplomatic_status(from_faction_id, to_faction_id) == DiplomacyStatus::Hostile;
}

bool Simulation::are_factions_mutual_friendly(Id a_faction_id, Id b_faction_id) const {
  if (a_faction_id == kInvalidId || b_faction_id == kInvalidId) return false;
  if (a_faction_id == b_faction_id) return true;
  return diplomatic_status(a_faction_id, b_faction_id) == DiplomacyStatus::Friendly &&
         diplomatic_status(b_faction_id, a_faction_id) == DiplomacyStatus::Friendly;
}

bool Simulation::set_diplomatic_status(Id from_faction_id, Id to_faction_id, DiplomacyStatus status, bool reciprocal,
                                       bool push_event_on_change) {
  if (from_faction_id == kInvalidId || to_faction_id == kInvalidId) return false;
  if (from_faction_id == to_faction_id) return false;

  auto* a = find_ptr(state_.factions, from_faction_id);
  auto* b = find_ptr(state_.factions, to_faction_id);
  if (!a || !b) return false;

  const auto prev_a = diplomatic_status(from_faction_id, to_faction_id);
  const auto prev_b = diplomatic_status(to_faction_id, from_faction_id);
  const bool was_mutual_friendly = (prev_a == DiplomacyStatus::Friendly && prev_b == DiplomacyStatus::Friendly);


  auto set_one = [](Faction& f, Id other, DiplomacyStatus st) {
    if (st == DiplomacyStatus::Hostile) {
      // Hostile is the implicit default; removing the entry keeps saves clean.
      f.relations.erase(other);
    } else {
      f.relations[other] = st;
    }
  };

  set_one(*a, to_faction_id, status);
  if (reciprocal) set_one(*b, from_faction_id, status);
  // If a mutual Friendly relationship was just established, immediately sync map
  // knowledge and contact intel between the two factions.
  const bool now_mutual_friendly = are_factions_mutual_friendly(from_faction_id, to_faction_id);
  if (now_mutual_friendly && !was_mutual_friendly) {
    int added_a_systems = 0;
    int added_b_systems = 0;
    int added_a_jumps = 0;
    int added_b_jumps = 0;
    int merged_a_contacts = 0;
    int merged_b_contacts = 0;

    auto merge_systems = [&](Faction& dst, const Faction& src, int& added) {
      for (Id sid : src.discovered_systems) {
        if (sid == kInvalidId) continue;
        if (std::find(dst.discovered_systems.begin(), dst.discovered_systems.end(), sid) != dst.discovered_systems.end()) {
          continue;
        }
        dst.discovered_systems.push_back(sid);
        added += 1;
      }
    };

    auto merge_contacts = [&](Faction& dst, const Faction& src, int& merged) {
      const auto keys = sorted_keys(src.ship_contacts);
      for (Id sid : keys) {
        const auto it_src = src.ship_contacts.find(sid);
        if (it_src == src.ship_contacts.end()) continue;
        const Contact& c = it_src->second;
        if (c.last_seen_faction_id == dst.id) continue;
        auto it_dst = dst.ship_contacts.find(sid);
        if (it_dst == dst.ship_contacts.end()) {
          dst.ship_contacts[sid] = c;
          merged += 1;
        } else if (c.last_seen_day > it_dst->second.last_seen_day) {
          it_dst->second = c;
          merged += 1;
        }
      }
    };

    auto merge_jump_surveys = [&](Faction& dst, const Faction& src, int& added) {
      for (Id jid : src.surveyed_jump_points) {
        if (jid == kInvalidId) continue;
        if (std::find(dst.surveyed_jump_points.begin(), dst.surveyed_jump_points.end(), jid) !=
            dst.surveyed_jump_points.end()) {
          continue;
        }
        dst.surveyed_jump_points.push_back(jid);
        added += 1;
      }
    };

    merge_systems(*a, *b, added_a_systems);
    merge_systems(*b, *a, added_b_systems);

    merge_jump_surveys(*a, *b, added_a_jumps);
    merge_jump_surveys(*b, *a, added_b_jumps);

    if (added_a_systems + added_b_systems + added_a_jumps + added_b_jumps > 0) invalidate_jump_route_cache();

    merge_contacts(*a, *b, merged_a_contacts);
    merge_contacts(*b, *a, merged_b_contacts);

    if (push_event_on_change) {
    if (added_a_systems > 0 || added_a_jumps > 0 || merged_a_contacts > 0) {
        EventContext ctx;
        ctx.faction_id = from_faction_id;
        ctx.faction_id2 = to_faction_id;
        const std::string msg = "Intel sharing with " + b->name + ": +" + std::to_string(added_a_systems) +
                                " systems, +" + std::to_string(added_a_jumps) + " jump surveys, +" +
                                std::to_string(merged_a_contacts) + " contacts";
        push_event(EventLevel::Info, EventCategory::Intel, msg, ctx);
      }
    if (added_b_systems > 0 || added_b_jumps > 0 || merged_b_contacts > 0) {
        EventContext ctx;
        ctx.faction_id = to_faction_id;
        ctx.faction_id2 = from_faction_id;
        const std::string msg = "Intel sharing with " + a->name + ": +" + std::to_string(added_b_systems) +
                                " systems, +" + std::to_string(added_b_jumps) + " jump surveys, +" +
                                std::to_string(merged_b_contacts) + " contacts";
        push_event(EventLevel::Info, EventCategory::Intel, msg, ctx);
      }
    }
  }


  if (push_event_on_change) {
    const bool changed_a = (prev_a != status);
    const bool changed_b = reciprocal && (prev_b != status);
    if (changed_a || changed_b) {
      const std::string status_str = (status == DiplomacyStatus::Friendly)
                                         ? "Friendly"
                                         : (status == DiplomacyStatus::Neutral ? "Neutral" : "Hostile");
      std::string msg;
      if (reciprocal) {
        msg = "Diplomacy: " + a->name + " and " + b->name + " are now " + status_str;
      } else {
        msg = "Diplomacy: " + a->name + " now views " + b->name + " as " + status_str;
      }
      EventContext ctx;
      ctx.faction_id = from_faction_id;
      ctx.faction_id2 = to_faction_id;
      push_event(EventLevel::Info, EventCategory::Diplomacy, msg, ctx);
    }
  }

  return true;
}

bool Simulation::is_ship_detected_by_faction(Id viewer_faction_id, Id target_ship_id) const {
  const auto* tgt = find_ptr(state_.ships, target_ship_id);
  if (!tgt) return false;
  if (tgt->faction_id == viewer_faction_id) return true;

  const auto sources = gather_sensor_sources(*this, viewer_faction_id, tgt->system_id);
  if (sources.empty()) return false;
  const auto* d = find_design(tgt->design_id);
  const double sig = sim_sensors::effective_signature_multiplier(*this, *tgt, d);
  return any_source_detects(sources, tgt->position_mkm, sig);
}

std::vector<Id> Simulation::detected_hostile_ships_in_system(Id viewer_faction_id, Id system_id) const {
  std::vector<Id> out;
  const auto* sys = find_ptr(state_.systems, system_id);
  if (!sys) return out;

  const auto sources = gather_sensor_sources(*this, viewer_faction_id, system_id);
  if (sources.empty()) return out;

  // Use a spatial index to avoid scanning every ship for every query.
  SpatialIndex2D idx;
  idx.build_from_ship_ids(sys->ships, state_.ships);

  const double max_sig = sim_sensors::max_signature_multiplier_for_detection(*this);

  for (const auto& src : sources) {
    if (src.range_mkm <= 1e-9) continue;
    const auto nearby = idx.query_radius(src.pos_mkm, src.range_mkm * max_sig, 1e-9);
    for (Id sid : nearby) {
      const auto* sh = find_ptr(state_.ships, sid);
      if (!sh) continue;
      if (sh->system_id != system_id) continue;
      if (sh->faction_id == viewer_faction_id) continue;
      if (!are_factions_hostile(viewer_faction_id, sh->faction_id)) continue;

      // Apply target signature/stealth + EMCON: effective detection range scales by the
      // target's signature multiplier. Values < 1.0 are harder to detect, values > 1.0 are
      // easier to detect.
      const auto* d = find_design(sh->design_id);
      const double sig = sim_sensors::effective_signature_multiplier(*this, *sh, d);
      const double eff = src.range_mkm * sig;
      if (eff <= 1e-9) continue;
      const double dx = sh->position_mkm.x - src.pos_mkm.x;
      const double dy = sh->position_mkm.y - src.pos_mkm.y;
      if (dx * dx + dy * dy > eff * eff + 1e-9) continue;

      out.push_back(sid);
    }
  }

  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::vector<Contact> Simulation::recent_contacts_in_system(Id viewer_faction_id, Id system_id, int max_age_days) const {
  std::vector<Contact> out;
  const auto* fac = find_ptr(state_.factions, viewer_faction_id);
  if (!fac) return out;

  const int now = static_cast<int>(state_.date.days_since_epoch());
  for (const auto& [_, c] : fac->ship_contacts) {
    if (c.system_id != system_id) continue;
    const int age = now - c.last_seen_day;
    if (age < 0) continue;
    if (age > max_age_days) continue;
    out.push_back(c);
  }

  std::sort(out.begin(), out.end(), [](const Contact& a, const Contact& b) {
    return a.last_seen_day > b.last_seen_day;
  });
  return out;
}


} // namespace nebula4x
