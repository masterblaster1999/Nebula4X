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
#include "nebula4x/core/procgen_nebula_microfield.h"
#include "nebula4x/core/procgen_nebula_stormfield.h"
#include "nebula4x/core/procgen_jump_phenomena.h"
#include "nebula4x/core/ai_economy.h"
#include "nebula4x/core/trade_network.h"
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
using sim_internal::trade_agreement_output_multiplier;
using sim_internal::sync_intel_between_factions;
using sim_internal::compute_power_allocation;

using sim_nav::PredictedNavState;
using sim_nav::predicted_nav_state_after_queued_jumps;

using sim_sensors::SensorSource;
using sim_sensors::gather_sensor_sources;
using sim_sensors::any_source_detects;

inline void normalize_faction_pair(Id& a, Id& b) {
  if (b < a) {
    const Id tmp = a;
    a = b;
    b = tmp;
  }
}

inline const char* treaty_type_title(TreatyType t) {
  switch (t) {
    case TreatyType::Ceasefire: return "Ceasefire";
    case TreatyType::NonAggressionPact: return "Non-Aggression Pact";
    case TreatyType::Alliance: return "Alliance";
    case TreatyType::TradeAgreement: return "Trade Agreement";
    case TreatyType::ResearchAgreement: return "Research Agreement";
  }
  return "Treaty";
}

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
  // Dijkstra cost used for route selection.
  //
  // This is an environment-adjusted distance (mkm / speed_multiplier), so routes
  // that traverse slow nebula/storm systems are naturally penalized.
  double cost_mkm{0.0};

  // Physical distance traveled inside systems (ignores environment multipliers).
  double distance_mkm{0.0};

  int hops{0};
};

bool dist_better(const JumpRouteDist& a, const JumpRouteDist& b) {
  constexpr double kEps = 1e-9;
  if (a.cost_mkm + kEps < b.cost_mkm) return true;
  if (std::fabs(a.cost_mkm - b.cost_mkm) <= kEps) {
    if (a.hops < b.hops) return true;
    if (a.hops == b.hops && a.distance_mkm + kEps < b.distance_mkm) return true;
  }
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
    if (a.d.distance_mkm != b.d.distance_mkm) return a.d.distance_mkm > b.d.distance_mkm;
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

  // Keep environment-adjusted totals consistent as well.
  plan.effective_total_distance_mkm = plan.effective_distance_mkm + plan.effective_final_leg_mkm;

  const double mkm_per_day = mkm_per_day_from_speed(speed_km_s, seconds_per_day);
  if (mkm_per_day <= 0.0) {
    plan.eta_days = std::numeric_limits<double>::infinity();
    plan.total_eta_days = std::numeric_limits<double>::infinity();
  } else {
    // eta_days and total_eta_days are derived from environment-adjusted distance.
    plan.eta_days = plan.effective_distance_mkm / mkm_per_day;
    plan.total_eta_days = plan.effective_total_distance_mkm / mkm_per_day;
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
    plan.effective_distance_mkm = 0.0;
    plan.has_goal_pos = has_goal;
    plan.arrival_pos_mkm = start_pos_mkm;
    if (has_goal) {
      plan.goal_pos_mkm = *goal_pos_mkm;
      plan.final_leg_mkm = (plan.goal_pos_mkm - plan.arrival_pos_mkm).length();

      // Use the same LOS-integrated environment cost model as real ship movement so
      // ETAs reflect nebula microfields / storm cells (not just system-average density).
      plan.effective_final_leg_mkm = sim.system_movement_environment_cost_los(
          start_system_id, plan.arrival_pos_mkm, plan.goal_pos_mkm, 0ULL);
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

  // Cache expensive in-system environment LOS costs while routing.
  //
  // Jump route planning can touch many (system, from, to) segments during Dijkstra,
  // and each LOS cost integrates through nebula microfields / storm cells. A small
  // per-solve cache keeps planning responsive without affecting determinism.
  struct SegmentCostKey {
    Id system_id{kInvalidId};
    std::uint64_t ax{0};
    std::uint64_t ay{0};
    std::uint64_t bx{0};
    std::uint64_t by{0};

    bool operator==(const SegmentCostKey& o) const {
      return system_id == o.system_id && ax == o.ax && ay == o.ay && bx == o.bx && by == o.by;
    }
  };

  struct SegmentCostKeyHash {
    std::size_t operator()(const SegmentCostKey& k) const noexcept {
      std::size_t h = static_cast<std::size_t>(k.system_id);
      auto mix = [&](std::uint64_t v) {
        h ^= static_cast<std::size_t>(v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
      };
      mix(k.ax);
      mix(k.ay);
      mix(k.bx);
      mix(k.by);
      return h;
    }
  };

  std::unordered_map<SegmentCostKey, double, SegmentCostKeyHash> seg_cost_cache;
  seg_cost_cache.reserve(256);

  auto seg_cost = [&](Id system_id, const Vec2& from_mkm, const Vec2& to_mkm) -> double {
    SegmentCostKey key;
    key.system_id = system_id;
    key.ax = canonical_double_bits(from_mkm.x);
    key.ay = canonical_double_bits(from_mkm.y);
    key.bx = canonical_double_bits(to_mkm.x);
    key.by = canonical_double_bits(to_mkm.y);

    if (auto it = seg_cost_cache.find(key); it != seg_cost_cache.end()) return it->second;

    const double c = sim.system_movement_environment_cost_los(system_id, from_mkm, to_mkm, 0ULL);
    seg_cost_cache.emplace(key, c);
    return c;
  };


  std::priority_queue<JumpRoutePQItem, std::vector<JumpRoutePQItem>, JumpRoutePQComp> pq;
  std::unordered_map<JumpRouteNode, JumpRouteDist, JumpRouteNodeHash> dist;
  std::unordered_map<JumpRouteNode, JumpRouteNode, JumpRouteNodeHash> prev;
  std::unordered_map<JumpRouteNode, Id, JumpRouteNodeHash> prev_jump;

  const JumpRouteNode start{start_system_id, kInvalidId};
  dist[start] = JumpRouteDist{0.0, 0.0, 0};
  prev[start] = JumpRouteNode{kInvalidId, kInvalidId};
  pq.push(JumpRoutePQItem{dist[start], start});

  JumpRouteNode best_goal{kInvalidId, kInvalidId};
  JumpRouteDist best_goal_dist{};
  double best_goal_total_cost = std::numeric_limits<double>::infinity();

  constexpr double kEps = 1e-9;

  auto goal_better = [&](double total_cost, const JumpRouteDist& cand_dist, const JumpRouteNode& cand_node) {
    if (total_cost + kEps < best_goal_total_cost) return true;
    if (std::fabs(total_cost - best_goal_total_cost) <= kEps) {
      // Tie-breaker: fewer hops, then lower travel-time cost, then shorter physical distance,
      // then deterministic ids.
      if (cand_dist.hops < best_goal_dist.hops) return true;
      if (cand_dist.hops == best_goal_dist.hops) {
        if (cand_dist.cost_mkm + kEps < best_goal_dist.cost_mkm) return true;
        if (std::fabs(cand_dist.cost_mkm - best_goal_dist.cost_mkm) <= kEps) {
          if (cand_dist.distance_mkm + kEps < best_goal_dist.distance_mkm) return true;
          if (std::fabs(cand_dist.distance_mkm - best_goal_dist.distance_mkm) <= kEps) {
            if (cand_node.entry_jump_id < best_goal.entry_jump_id) return true;
          }
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
      double terminal = has_goal ? seg_cost(target_system_id, cur_pos, *goal_pos_mkm) : 0.0;
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

      // Environment-adjusted distance for the in-system leg to this jump point.
      // Uses LOS integration so microfields / storm cells affect routing choices and ETA.
      const double eff_leg = seg_cost(cur.node.system_id, cur_pos, jp_pos);

      const JumpRouteNode nxt{next_sys, dest_jp->id};
      const JumpRouteDist cand{cur.d.cost_mkm + eff_leg, cur.d.distance_mkm + leg, cur.d.hops + 1};

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
  if (it_goal != dist.end()) {
    plan.distance_mkm = it_goal->second.distance_mkm;
    plan.effective_distance_mkm = it_goal->second.cost_mkm;
  }

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

  // Environment adjustment for the final in-system leg (0 when has_goal == false).
  plan.effective_final_leg_mkm = has_goal ? seg_cost(target_system_id, plan.arrival_pos_mkm, plan.goal_pos_mkm) : 0.0;

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

double Simulation::reverse_engineering_points_required_for_component(const std::string& component_id) const {
  const auto it = content_.components.find(component_id);
  if (it == content_.components.end()) return 0.0;

  const double mass_tons = std::max(0.0, it->second.mass_tons);
  const double per_ton = std::max(0.0, cfg_.reverse_engineering_points_required_per_component_ton);
  // Ensure non-zero thresholds so tiny/zero-mass components don't unlock instantly.
  return std::max(1.0, mass_tons * per_ton);
}


double Simulation::ship_effective_signature_multiplier(const Ship& ship, const ShipDesign* design) const {
  if (!design) design = find_design(ship.design_id);
  return sim_sensors::effective_signature_multiplier(*this, ship, design);
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
    const double trade = trade_agreement_output_multiplier(state_, fac->id);
    total *= std::max(0.0, m.construction) * std::max(0.0, trade);
  }

  // Interstellar trade prosperity bonus (system market access / hub activity).
  total *= trade_prosperity_output_multiplier_for_colony(colony.id);

  // Local temporary conditions (strikes, accidents, festivals, etc.).
  total *= colony_condition_multipliers(colony).construction;

  // Colony stability output scaling (optional).
  total *= colony_stability_output_multiplier_for_colony(colony);

  // Blockade output disruption.
  if (cfg_.enable_blockades) total *= blockade_output_multiplier_for_colony(colony.id);

  return std::max(0.0, total);
}

double Simulation::body_habitability(Id body_id) const {
  return body_habitability_for_faction(body_id, kInvalidId);
}

double Simulation::body_habitability_for_faction(Id body_id, Id faction_id) const {
  if (!cfg_.enable_habitability) return 1.0;

  const auto* b = find_ptr(state_.bodies, body_id);
  if (!b) return 1.0;

  // If terraforming is marked complete, treat as fully habitable.
  if (b->terraforming_complete) return 1.0;

  // Base ideal/tolerance from SimConfig, optionally overridden by the faction's
  // SpeciesProfile.
  double ideal_t = cfg_.habitability_ideal_temp_k;
  double ideal_atm = cfg_.habitability_ideal_atm;
  double ideal_o2 = cfg_.habitability_ideal_o2_atm;
  double t_tol = cfg_.habitability_temp_tolerance_k;
  double a_tol = cfg_.habitability_atm_tolerance;
  double o2_tol = cfg_.habitability_o2_tolerance_atm;

  if (faction_id != kInvalidId) {
    if (const auto* fac = find_ptr(state_.factions, faction_id)) {
      const SpeciesProfile& sp = fac->species;
      if (sp.ideal_temp_k > 0.0) ideal_t = sp.ideal_temp_k;
      if (sp.ideal_atm > 0.0) ideal_atm = sp.ideal_atm;
      if (sp.ideal_o2_atm > 0.0) ideal_o2 = sp.ideal_o2_atm;
      if (sp.temp_tolerance_k > 0.0) t_tol = sp.temp_tolerance_k;
      if (sp.atm_tolerance > 0.0) a_tol = sp.atm_tolerance;
      if (sp.o2_tolerance_atm > 0.0) o2_tol = sp.o2_tolerance_atm;
    }
  }

  // Use terraforming targets as the "ideal" environment when present.
  // This makes partial terraforming gradually improve habitability.
  if (b->terraforming_target_temp_k > 0.0) ideal_t = b->terraforming_target_temp_k;
  if (b->terraforming_target_atm > 0.0) ideal_atm = b->terraforming_target_atm;
  if (b->terraforming_target_o2_atm > 0.0) ideal_o2 = b->terraforming_target_o2_atm;

  const double dt = std::fabs(b->surface_temp_k - ideal_t);
  const double da = std::fabs(b->atmosphere_atm - ideal_atm);

  // Oxygen is optional: only incorporate it when the body has oxygen metadata
  // or the user has set an explicit terraforming oxygen target.
  const bool use_o2 = (b->terraforming_target_o2_atm > 0.0) || (b->oxygen_atm > 0.0);
  const double o2 = std::clamp(b->oxygen_atm, 0.0, std::max(0.0, b->atmosphere_atm));
  const double do2 = std::fabs(o2 - ideal_o2);

  auto factor_linear = [](double delta, double tol) {
    if (!(tol > 0.0)) return (delta <= 0.0) ? 1.0 : 0.0;
    const double x = 1.0 - (delta / tol);
    return std::clamp(x, 0.0, 1.0);
  };

  t_tol = std::max(1e-9, t_tol);
  a_tol = std::max(1e-9, a_tol);
  o2_tol = std::max(1e-9, o2_tol);

  const double t_factor = factor_linear(dt, t_tol);
  const double a_factor = factor_linear(da, a_tol);

  double o2_factor = 1.0;
  if (use_o2) {
    o2_factor = factor_linear(do2, o2_tol);
    // Simple safety constraint: O2 fraction must not exceed a configurable limit.
    if (b->atmosphere_atm > 1e-9) {
      const double frac = o2 / b->atmosphere_atm;
      if (frac > cfg_.habitability_o2_max_fraction_of_atm + 1e-12) o2_factor = 0.0;
    }
  }

  return std::clamp(t_factor * a_factor * o2_factor, 0.0, 1.0);
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

  const double hab = body_habitability_for_faction(colony.body_id, colony.faction_id);
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
    const double trade = trade_agreement_output_multiplier(state_, fac->id);
    return std::max(0.0, m.shipyard) * std::max(0.0, trade);
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

      // Rearm needs: enough Munitions at the colony to top up docked ships' missile ammo.
      {
        constexpr const char* kMunitions = "Munitions";
        double desired_munitions = 0.0;

        for (Id sid : ship_ids) {
          const Ship* ship = find_ptr(state_.ships, sid);
          if (!ship) continue;
          if (ship->faction_id != faction_id) continue;
          if (ship->system_id != body->system_id) continue;

          const double dist = (ship->position_mkm - body->position_mkm).length();
          if (dist > dock_range + 1e-9) continue;

          const ShipDesign* d = find_design(ship->design_id);
          if (!d) continue;
          const int cap = std::max(0, d->missile_ammo_capacity);
          if (cap <= 0) continue;

          int have_ship = ship->missile_ammo;
          if (have_ship < 0) have_ship = cap;
          have_ship = std::clamp(have_ship, 0, cap);
          const int need = cap - have_ship;
          if (need > 0) desired_munitions += static_cast<double>(need);
        }

        if (desired_munitions > 1e-9) {
          const double have = [&]() {
            if (auto it2 = colony->minerals.find(kMunitions); it2 != colony->minerals.end()) return it2->second;
            return 0.0;
          }();

          LogisticsNeed n;
          n.colony_id = cid;
          n.kind = LogisticsNeedKind::Rearm;
          n.mineral = kMunitions;
          n.desired_tons = desired_munitions;
          n.have_tons = have;
          n.missing_tons = std::max(0.0, desired_munitions - have);
          out.push_back(std::move(n));
        }
      }

      // Maintenance needs: keep a buffer of maintenance supplies at the colony for docked ships.
      if (cfg_.enable_ship_maintenance && cfg_.ship_maintenance_tons_per_day_per_mass_ton > 0.0 &&
          !cfg_.ship_maintenance_resource_id.empty()) {
        const std::string& res = cfg_.ship_maintenance_resource_id;

        double per_day = 0.0;
        for (Id sid : ship_ids) {
          const Ship* ship = find_ptr(state_.ships, sid);
          if (!ship) continue;
          if (ship->faction_id != faction_id) continue;
          if (ship->system_id != body->system_id) continue;

          const double dist = (ship->position_mkm - body->position_mkm).length();
          if (dist > dock_range + 1e-9) continue;

          const ShipDesign* d = find_design(ship->design_id);
          if (!d) continue;
          const double mass = std::max(0.0, d->mass_tons);
          per_day += mass * cfg_.ship_maintenance_tons_per_day_per_mass_ton;
        }
        const double desired_maint = per_day * buffer_days;

        if (desired_maint > 1e-9) {
          const double have = [&]() {
            if (auto it2 = colony->minerals.find(res); it2 != colony->minerals.end()) return it2->second;
            return 0.0;
          }();

          LogisticsNeed n;
          n.colony_id = cid;
          n.kind = LogisticsNeedKind::Maintenance;
          n.mineral = res;
          n.desired_tons = desired_maint;
          n.have_tons = have;
          n.missing_tons = std::max(0.0, desired_maint - have);
          out.push_back(std::move(n));
        }
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

double Simulation::jump_survey_required_points_for_jump(Id jump_point_id) const {
  const double base = cfg_.jump_survey_points_required;
  if (base <= 1e-9) return base;
  if (jump_point_id == kInvalidId) return base;
  if (!cfg_.enable_jump_point_phenomena) return base;

  const double strength = std::clamp(cfg_.jump_phenomena_survey_difficulty_strength, 0.0, 3.0);
  if (strength <= 1e-9) return base;

  const auto* jp = find_ptr(state_.jump_points, jump_point_id);
  if (!jp) return base;

  const auto ph = procgen_jump_phenomena::generate(*jp);
  double mult = ph.survey_difficulty_mult;
  if (!std::isfinite(mult) || mult <= 0.0) mult = 1.0;
  mult = std::clamp(mult, 0.25, 10.0);

  const double eff_mult = 1.0 + (mult - 1.0) * strength;
  double required = base * eff_mult;
  if (!std::isfinite(required)) required = base;
  return std::max(0.0, required);
}

bool Simulation::is_anomaly_discovered_by_faction(Id viewer_faction_id, Id anomaly_id) const {
  if (anomaly_id == kInvalidId) return false;
  // When there is no "viewer" (e.g. omniscient mode / tools), treat all anomalies as known.
  if (viewer_faction_id == kInvalidId) return true;
  const auto* fac = find_ptr(state_.factions, viewer_faction_id);
  if (!fac) return true;
  return std::find(fac->discovered_anomalies.begin(), fac->discovered_anomalies.end(), anomaly_id) !=
         fac->discovered_anomalies.end();
}



// --- Environmental helpers (nebula + storms) ---

bool Simulation::system_has_storm(Id system_id) const {
  const auto* sys = find_ptr(state_.systems, system_id);
  if (!sys) return false;
  if (!(sys->storm_peak_intensity > 0.0)) return false;
  if (sys->storm_end_day <= sys->storm_start_day) return false;

  const double now = static_cast<double>(state_.date.days_since_epoch()) +
                     static_cast<double>(state_.hour_of_day) / 24.0;
  return now >= static_cast<double>(sys->storm_start_day) &&
         now < static_cast<double>(sys->storm_end_day);
}

double Simulation::system_storm_intensity(Id system_id) const {
  const auto* sys = find_ptr(state_.systems, system_id);
  if (!sys) return 0.0;
  if (!(sys->storm_peak_intensity > 0.0)) return 0.0;
  if (sys->storm_end_day <= sys->storm_start_day) return 0.0;

  const double now = static_cast<double>(state_.date.days_since_epoch()) +
                     static_cast<double>(state_.hour_of_day) / 24.0;
  const double start = static_cast<double>(sys->storm_start_day);
  const double end = static_cast<double>(sys->storm_end_day);
  if (now < start || now >= end) return 0.0;

  const double dur = end - start;
  if (dur <= 0.0) return 0.0;

  // Smooth pulse: 0 -> 1 -> 0 across the storm window.
  double t = (now - start) / dur;
  t = std::clamp(t, 0.0, 1.0);
  constexpr double kPi = 3.141592653589793238462643383279502884;
  const double pulse = std::sin(kPi * t);

  const double peak = std::clamp(sys->storm_peak_intensity, 0.0, 1.0);
  return std::clamp(peak * std::max(0.0, pulse), 0.0, 1.0);
}

double Simulation::system_storm_intensity_at(Id system_id, const Vec2& pos_mkm) const {
  // Base (system-wide) storm pulse.
  const double base = system_storm_intensity(system_id);
  if (base <= 0.0) return 0.0;
  if (!cfg_.enable_nebula_storm_cells) return base;

  const auto* sys = find_ptr(state_.systems, system_id);
  if (!sys) return base;

  // Compute storm-relative time (days) to keep the procedural field numerically stable.
  const double now = static_cast<double>(state_.date.days_since_epoch()) +
                     static_cast<double>(state_.hour_of_day) / 24.0;
  const double start = static_cast<double>(sys->storm_start_day);
  const double end = static_cast<double>(sys->storm_end_day);
  if (now < start || now >= end) return 0.0;
  const double age = now - start;

  // Deterministic per-storm seed (system id + storm window).
  std::uint64_t seed = procgen_obscure::splitmix64(static_cast<std::uint64_t>(system_id) ^ 0x6C8E9CF570932BD5ULL);
  seed = procgen_obscure::splitmix64(seed ^ static_cast<std::uint64_t>(sys->storm_start_day));
  seed = procgen_obscure::splitmix64(seed ^ static_cast<std::uint64_t>(sys->storm_end_day));

  procgen_nebula_stormfield::Params p;
  p.cell_scale_mkm = cfg_.nebula_storm_cell_scale_mkm;
  p.drift_speed_mkm_per_day = cfg_.nebula_storm_cell_drift_speed_mkm_per_day;
  // Map the single "sharpness" knob into both the underlying noise curve and
  // the final contrast curve used for cells.
  const double sh = std::clamp(cfg_.nebula_storm_cell_sharpness, 0.25, 4.0);
  p.sharpness = sh;
  p.cell_contrast = std::clamp(0.85 * sh, 0.25, 6.0);

  // Keep the storm look consistent (these are intentionally not exposed as config knobs yet).
  p.filament_mix = 0.55;
  p.cell_threshold = 0.30;
  p.swirl_strength = 0.18;
  p.swirl_scale_mkm = 8000.0;

  const double v = procgen_nebula_stormfield::sample_cell01(seed, pos_mkm, age, p);
  const double strength = std::clamp(cfg_.nebula_storm_cell_strength, 0.0, 1.5);

  // Remap around 1.0 so the system-wide intensity remains the average.
  double factor = 1.0 + strength * (v - 0.5) * 2.0;
  if (!std::isfinite(factor)) factor = 1.0;
  factor = std::clamp(factor, 0.0, 2.0);

  return std::clamp(base * factor, 0.0, 1.0);
}

double Simulation::system_sensor_environment_multiplier(Id system_id) const {
  const auto* sys = find_ptr(state_.systems, system_id);
  if (!sys) return 1.0;

  const double neb = std::clamp(sys->nebula_density, 0.0, 1.0);
  const double base = std::clamp(1.0 - 0.65 * neb, 0.25, 1.0);

  if (!cfg_.enable_nebula_storms) return base;

  const double storm = system_storm_intensity(system_id);
  const double pen = std::max(0.0, cfg_.nebula_storm_sensor_penalty);
  const double storm_mult = std::clamp(1.0 - pen * storm, 0.05, 1.0);

  return std::clamp(base * storm_mult, 0.05, 1.0);
}

double Simulation::system_movement_speed_multiplier(Id system_id) const {
  const auto* sys = find_ptr(state_.systems, system_id);
  if (!sys) return 1.0;

  const double neb = std::clamp(sys->nebula_density, 0.0, 1.0);
  const double storm = (cfg_.enable_nebula_storms ? system_storm_intensity(system_id) : 0.0);

  double m = 1.0;

  if (cfg_.enable_nebula_drag) {
    const double drag = std::max(0.0, cfg_.nebula_drag_speed_penalty_at_max_density);
    m *= std::clamp(1.0 - drag * neb, 0.05, 1.0);
  }

  if (cfg_.enable_nebula_storms) {
    const double pen = std::max(0.0, cfg_.nebula_storm_speed_penalty);
    m *= std::clamp(1.0 - pen * storm, 0.05, 1.0);
  }

  return std::clamp(m, 0.05, 1.0);
}


double Simulation::system_nebula_density_at(Id system_id, const Vec2& pos_mkm) const {
  const auto* sys = find_ptr(state_.systems, system_id);
  if (!sys) return 0.0;

  const double base = std::clamp(sys->nebula_density, 0.0, 1.0);
  if (!cfg_.enable_nebula_microfields) return base;
  if (base <= 1e-6) return 0.0;

  procgen_nebula_microfield::Params p;
  p.scale_mkm = cfg_.nebula_microfield_scale_mkm;
  p.warp_scale_mkm = cfg_.nebula_microfield_warp_scale_mkm;
  p.strength = cfg_.nebula_microfield_strength;
  p.filament_mix = cfg_.nebula_microfield_filament_mix;
  p.sharpness = cfg_.nebula_microfield_sharpness;

  // Deterministic per-system seed. We intentionally avoid save-specific RNG
  // state so the field is stable and cacheable.
  std::uint64_t seed = procgen_obscure::splitmix64(static_cast<std::uint64_t>(system_id) ^ 0x9E3779B97F4A7C15ULL);

  return procgen_nebula_microfield::local_density(base, seed, pos_mkm, p);
}

double Simulation::system_sensor_environment_multiplier_at(Id system_id, const Vec2& pos_mkm) const {
  const auto* sys = find_ptr(state_.systems, system_id);
  if (!sys) return 1.0;

  const double neb = std::clamp(system_nebula_density_at(system_id, pos_mkm), 0.0, 1.0);
  const double base = std::clamp(1.0 - 0.65 * neb, 0.25, 1.0);

  if (!cfg_.enable_nebula_storms) return base;

  const double storm = system_storm_intensity_at(system_id, pos_mkm);
  const double pen = std::max(0.0, cfg_.nebula_storm_sensor_penalty);
  const double storm_mult = std::clamp(1.0 - pen * storm, 0.05, 1.0);

  return std::clamp(base * storm_mult, 0.05, 1.0);
}

double Simulation::system_sensor_environment_multiplier_los(Id system_id, const Vec2& from_mkm, const Vec2& to_mkm,
                                                           std::uint64_t extra_seed) const {
  if (!cfg_.enable_sensor_los_attenuation) return 1.0;
  const auto* sys = find_ptr(state_.systems, system_id);
  if (!sys) return 1.0;

  // Only pay the cost when there is potential *spatial* variation along the ray.
  // If the environment is uniform, endpoint multipliers already capture the effect.
  const bool micro = (cfg_.enable_nebula_microfields && sys->nebula_density > 1e-6 && cfg_.nebula_microfield_strength > 1e-9);
  const bool storm_cells = (cfg_.enable_nebula_storms && cfg_.enable_nebula_storm_cells && system_has_storm(system_id) &&
                            cfg_.nebula_storm_cell_strength > 1e-9 && cfg_.nebula_storm_sensor_penalty > 1e-9);
  if (!micro && !storm_cells) return 1.0;

  const Vec2 d = to_mkm - from_mkm;
  const double L = std::sqrt(d.x * d.x + d.y * d.y);
  if (!std::isfinite(L) || L <= 1e-6) return 1.0;

  const Vec2 dir{d.x / L, d.y / L};

  auto env_at = [&](const Vec2& p) -> double {
    return std::clamp(this->system_sensor_environment_multiplier_at(system_id, p), 0.0, 1.0);
  };

  const double env0 = env_at(from_mkm);
  const double env1 = env_at(to_mkm);
  double baseline = std::sqrt(std::max(0.0, env0) * std::max(0.0, env1));
  baseline = std::clamp(baseline, 1e-6, 1.0);

  // Cheap early-out: if the midpoint matches the endpoints closely, the LOS is likely uniform.
  const Vec2 mid = from_mkm + dir * (0.5 * L);
  const double env_mid = env_at(mid);
  if (std::abs(env_mid - baseline) < 1e-4 && std::abs(env0 - env1) < 1e-4) return 1.0;

  // Deterministic per-query seed. We quantize positions to avoid bit-level float noise.
  auto q = [](double v) -> std::int64_t {
    // 1/16 mkm (62,500 km) grid: stable but still responsive.
    return static_cast<std::int64_t>(std::llround(v * 16.0));
  };

  std::uint64_t seed = 0xA5F04C9B3E11D7A1ULL;
  seed ^= static_cast<std::uint64_t>(system_id) * 0x9E3779B97F4A7C15ULL;
  seed ^= static_cast<std::uint64_t>(q(from_mkm.x)) * 0xBF58476D1CE4E5B9ULL;
  seed ^= static_cast<std::uint64_t>(q(from_mkm.y)) * 0x94D049BB133111EBULL;
  seed ^= static_cast<std::uint64_t>(q(to_mkm.x)) * 0xD6E8FEB86659FD93ULL;
  seed ^= static_cast<std::uint64_t>(q(to_mkm.y)) * 0x2545F4914F6CDD1DULL;
  seed ^= procgen_obscure::splitmix64(extra_seed ^ 0x3C79AC492BA7B653ULL);
  seed = procgen_obscure::splitmix64(seed);

  auto rand01 = [&](std::uint64_t salt) -> double {
    return procgen_obscure::u01_from_u64(procgen_obscure::splitmix64(seed ^ salt));
  };

  // Config knobs (clamped to sane ranges).
  const double iso_env = std::clamp(cfg_.sensor_los_iso_env, 0.0, 1.0);
  const double step_scale = std::clamp(cfg_.sensor_los_sdf_step_scale, 0.05, 1.0);
  const double min_step = std::max(1e-3, cfg_.sensor_los_min_step_mkm);
  const double max_step = std::max(min_step, cfg_.sensor_los_max_step_mkm);
  const double eps = std::max(1e-3, cfg_.sensor_los_grad_epsilon_mkm);
  const int max_steps = std::clamp(cfg_.sensor_los_max_steps, 4, 512);
  const double jitter = std::clamp(cfg_.sensor_los_sample_jitter, 0.0, 0.49);

  const double min_mult = std::clamp(cfg_.sensor_los_min_multiplier, 0.0, 1.0);
  const double max_mult = std::max(min_mult, std::clamp(cfg_.sensor_los_max_multiplier, 0.0, 4.0));
  const double strength = std::max(0.0, cfg_.sensor_los_strength);

  // Adaptive ray-march integration of env along the segment.
  //
  // We treat the scalar field f(p) = env_at(p) as an implicit surface via
  // h(p) = f(p) - iso_env and use a signed-distance *estimate* sd ~= h/|grad h|.
  // This is not a true SDF, but it provides a useful step size heuristic.
  double sum_env_ds = 0.0;
  double s = 0.0;

  // Small deterministic start offset reduces banding when many queries share similar geometry.
  {
    const double off = (rand01(0xBADC0FFEEULL) - 0.5) * min_step;
    if (std::isfinite(off)) s = std::clamp(off, 0.0, 0.75 * min_step);
  }

  for (int step = 0; step < max_steps && s < L - 1e-9; ++step) {
    const Vec2 p = from_mkm + dir * s;
    const double f = env_at(p);
    const double h = f - iso_env;

    // Central-difference gradient of h.
    const double fxp = env_at(Vec2{p.x + eps, p.y});
    const double fxm = env_at(Vec2{p.x - eps, p.y});
    const double fyp = env_at(Vec2{p.x, p.y + eps});
    const double fym = env_at(Vec2{p.x, p.y - eps});
    const double gx = (fxp - fxm) / (2.0 * eps);
    const double gy = (fyp - fym) / (2.0 * eps);
    const double grad = std::sqrt(gx * gx + gy * gy);

    double dist_est = max_step;
    if (std::isfinite(grad) && grad > 1e-9) {
      dist_est = std::abs(h) / grad;
    }

    double ds = std::clamp(dist_est * step_scale, min_step, max_step);
    if (s + ds > L) ds = L - s;

    // Stratified sample within the step (deterministic jitter).
    double u = (rand01(0xC0FFEEULL + static_cast<std::uint64_t>(step)) - 0.5) * 2.0;
    u *= jitter;
    double t = std::clamp(0.5 + u, 0.0, 1.0);
    const Vec2 ps = from_mkm + dir * (s + ds * t);

    const double env_s = env_at(ps);
    sum_env_ds += env_s * ds;

    s += ds;
  }

  if (!std::isfinite(sum_env_ds) || sum_env_ds <= 1e-12) return 1.0;

  double avg_env = sum_env_ds / L;
  if (!std::isfinite(avg_env)) return 1.0;
  avg_env = std::clamp(avg_env, 0.0, 1.0);

  // Normalize to endpoints to avoid double-counting. Clamp by design: by default
  // this only *reduces* detection (max_mult == 1).
  double rel = avg_env / baseline;
  if (!std::isfinite(rel)) rel = 1.0;
  rel = std::clamp(rel, min_mult, max_mult);

  if (strength <= 1e-9) return 1.0;

  double out = std::pow(rel, strength);
  if (!std::isfinite(out)) out = 1.0;
  out = std::clamp(out, min_mult, max_mult);
  return out;
}





double Simulation::system_beam_environment_multiplier_los(Id system_id, const Vec2& from_mkm, const Vec2& to_mkm,
                                                         std::uint64_t extra_seed) const {
  if (!cfg_.enable_beam_los_attenuation) return 1.0;
  const auto* sys = find_ptr(state_.systems, system_id);
  if (!sys) return 1.0;

  const double strength = std::max(0.0, cfg_.beam_los_strength);
  if (strength <= 1e-9) return 1.0;

  const Vec2 d = to_mkm - from_mkm;
  const double L = std::sqrt(d.x * d.x + d.y * d.y);
  if (!std::isfinite(L) || L <= 1e-6) return 1.0;

  // Interpret the local sensor environment multiplier field as a beam
  // transmission field. This ensures beams "feel" the same nebula microfields
  // / storm cells that already affect detection, without introducing a second
  // procedural field.
  auto env_at = [&](const Vec2& p) -> double {
    return std::clamp(this->system_sensor_environment_multiplier_at(system_id, p), 0.0, 1.0);
  };

  // Only pay the ray-march cost when there is spatial variation along the ray.
  // If the environment is uniform, we can use the system-wide multiplier.
  const bool micro = (cfg_.enable_nebula_microfields && sys->nebula_density > 1e-6 && cfg_.nebula_microfield_strength > 1e-9);
  const bool storm_cells = (cfg_.enable_nebula_storms && cfg_.enable_nebula_storm_cells && system_has_storm(system_id) &&
                            cfg_.nebula_storm_cell_strength > 1e-9 && cfg_.nebula_storm_sensor_penalty > 1e-9);

  const double min_mult = std::clamp(cfg_.beam_los_min_multiplier, 0.0, 1.0);
  const double max_mult = std::max(min_mult, std::clamp(cfg_.beam_los_max_multiplier, 0.0, 4.0));

  if (!micro && !storm_cells) {
    double m = std::clamp(this->system_sensor_environment_multiplier(system_id), 0.0, 1.0);
    m = std::clamp(m, min_mult, max_mult);
    double out = std::pow(m, strength);
    if (!std::isfinite(out)) out = 1.0;
    return std::clamp(out, min_mult, max_mult);
  }

  const Vec2 dir{d.x / L, d.y / L};

  const double env0 = env_at(from_mkm);
  const double env1 = env_at(to_mkm);
  double baseline = std::sqrt(std::max(0.0, env0) * std::max(0.0, env1));
  baseline = std::clamp(baseline, min_mult, max_mult);

  // Cheap early-out: if the midpoint matches the endpoints closely, the LOS is likely uniform.
  const Vec2 mid = from_mkm + dir * (0.5 * L);
  const double env_mid = env_at(mid);
  if (std::abs(env_mid - baseline) < 1e-4 && std::abs(env0 - env1) < 1e-4) {
    double out = std::pow(baseline, strength);
    if (!std::isfinite(out)) out = 1.0;
    return std::clamp(out, min_mult, max_mult);
  }

  // Deterministic per-query seed. We quantize positions to avoid bit-level float noise.
  auto q = [](double v) -> std::int64_t {
    // 1/16 mkm (62,500 km) grid: stable but still responsive.
    return static_cast<std::int64_t>(std::llround(v * 16.0));
  };

  std::uint64_t seed = 0xBEEFBEEFCAFED00DULL;
  seed ^= static_cast<std::uint64_t>(system_id) * 0x9E3779B97F4A7C15ULL;
  seed ^= static_cast<std::uint64_t>(q(from_mkm.x)) * 0xBF58476D1CE4E5B9ULL;
  seed ^= static_cast<std::uint64_t>(q(from_mkm.y)) * 0x94D049BB133111EBULL;
  seed ^= static_cast<std::uint64_t>(q(to_mkm.x)) * 0xD6E8FEB86659FD93ULL;
  seed ^= static_cast<std::uint64_t>(q(to_mkm.y)) * 0x2545F4914F6CDD1DULL;
  seed ^= procgen_obscure::splitmix64(extra_seed ^ 0x4E7D73A8B9C6D2E1ULL);
  seed = procgen_obscure::splitmix64(seed);

  auto rand01 = [&](std::uint64_t salt) -> double {
    return procgen_obscure::u01_from_u64(procgen_obscure::splitmix64(seed ^ salt));
  };

  // Config knobs (clamped to sane ranges).
  const double iso_env = std::clamp(cfg_.beam_los_iso_env, 0.0, 1.0);
  const double step_scale = std::clamp(cfg_.beam_los_sdf_step_scale, 0.05, 1.0);
  const double min_step = std::max(1e-3, cfg_.beam_los_min_step_mkm);
  const double max_step = std::max(min_step, cfg_.beam_los_max_step_mkm);
  const double eps = std::max(1e-3, cfg_.beam_los_grad_epsilon_mkm);
  const int max_steps = std::clamp(cfg_.beam_los_max_steps, 4, 512);
  const double jitter = std::clamp(cfg_.beam_los_sample_jitter, 0.0, 0.49);

  // Adaptive ray-march integration of transmission along the segment.
  //
  // We treat f(p) = env_at(p) as an implicit surface via h(p) = f(p) - iso_env
  // and use a signed-distance *estimate* sd ~= h/|grad h| to choose conservative
  // step sizes (sphere-tracing style). This is not a true SDF, but works well as
  // a heuristic around sharp nebula fronts.
  double sum_env_ds = 0.0;
  double sum_log_env_ds = 0.0;
  double s = 0.0;

  // Small deterministic start offset reduces banding when many shots share similar geometry.
  {
    const double off = (rand01(0xDEADBEEFULL) - 0.5) * min_step;
    if (std::isfinite(off)) s = std::clamp(off, 0.0, 0.75 * min_step);
  }

  for (int step = 0; step < max_steps && s < L - 1e-9; ++step) {
    const Vec2 p = from_mkm + dir * s;
    const double f = env_at(p);
    const double h = f - iso_env;

    // Central-difference gradient of h (same as gradient of f).
    const double fxp = env_at(Vec2{p.x + eps, p.y});
    const double fxm = env_at(Vec2{p.x - eps, p.y});
    const double fyp = env_at(Vec2{p.x, p.y + eps});
    const double fym = env_at(Vec2{p.x, p.y - eps});
    const double gx = (fxp - fxm) / (2.0 * eps);
    const double gy = (fyp - fym) / (2.0 * eps);
    const double grad = std::sqrt(gx * gx + gy * gy);

    double dist_est = max_step;
    if (std::isfinite(grad) && grad > 1e-9) {
      dist_est = std::abs(h) / grad;
    }

    double ds = std::clamp(dist_est * step_scale, min_step, max_step);
    if (s + ds > L) ds = L - s;

    // Stratified sample within the step (deterministic jitter).
    double u = (rand01(0xC0FFEEULL + static_cast<std::uint64_t>(step)) - 0.5) * 2.0;
    u *= jitter;
    double t = std::clamp(0.5 + u, 0.0, 1.0);
    const Vec2 ps = from_mkm + dir * (s + ds * t);

    const double env_s = env_at(ps);
    sum_env_ds += env_s * ds;

    // Log integration gives geometric-mean transmission (multiplicative medium).
    const double safe = std::clamp(env_s, 1e-6, 1.0);
    sum_log_env_ds += std::log(safe) * ds;

    s += ds;
  }

  double trans = baseline;
  if (cfg_.beam_los_use_geometric_mean) {
    if (std::isfinite(sum_log_env_ds)) {
      const double avg_log = sum_log_env_ds / L;
      const double gm = std::exp(avg_log);
      if (std::isfinite(gm)) trans = std::clamp(gm, 0.0, 1.0);
    }
  } else {
    if (std::isfinite(sum_env_ds) && sum_env_ds > 1e-12) {
      double avg = sum_env_ds / L;
      if (std::isfinite(avg)) trans = std::clamp(avg, 0.0, 1.0);
    }
  }

  trans = std::clamp(trans, min_mult, max_mult);

  double out = std::pow(trans, strength);
  if (!std::isfinite(out)) out = 1.0;
  out = std::clamp(out, min_mult, max_mult);
  return out;
}



double Simulation::system_movement_environment_cost_los(Id system_id, const Vec2& from_mkm, const Vec2& to_mkm,
                                                      std::uint64_t extra_seed) const {
  const auto* sys = find_ptr(state_.systems, system_id);
  const Vec2 d = to_mkm - from_mkm;
  const double L = std::sqrt(d.x * d.x + d.y * d.y);
  if (!sys) return (std::isfinite(L) ? L : 0.0);
  if (!std::isfinite(L) || L <= 1e-6) return 0.0;

  // Only pay the ray-march cost when there is spatial variation that can
  // influence movement. Microfields only matter when nebula drag is enabled.
  const bool micro = (cfg_.enable_nebula_drag && cfg_.enable_nebula_microfields && sys->nebula_density > 1e-6 &&
                      cfg_.nebula_microfield_strength > 1e-9 && cfg_.nebula_drag_speed_penalty_at_max_density > 1e-9);
  const bool storm_cells = (cfg_.enable_nebula_storms && cfg_.enable_nebula_storm_cells && system_has_storm(system_id) &&
                            cfg_.nebula_storm_cell_strength > 1e-9 && cfg_.nebula_storm_speed_penalty > 1e-9);
  if (!micro && !storm_cells) {
    const double m = std::max(1e-6, this->system_movement_speed_multiplier(system_id));
    return L / m;
  }

  const Vec2 dir{d.x / L, d.y / L};

  auto speed_at = [&](const Vec2& p) -> double {
    return std::clamp(this->system_movement_speed_multiplier_at(system_id, p), 0.05, 1.0);
  };

  // Cheap early-out: if the midpoint matches endpoints closely, the segment is
  // likely uniform enough that a simple average is sufficient.
  const double m0 = speed_at(from_mkm);
  const double m1 = speed_at(to_mkm);
  const double baseline = std::clamp(std::sqrt(m0 * m1), 0.05, 1.0);
  const Vec2 mid = from_mkm + dir * (0.5 * L);
  const double mm = speed_at(mid);
  if (std::abs(mm - baseline) < 1e-4 && std::abs(m0 - m1) < 1e-4) {
    return L / baseline;
  }

  // Deterministic per-query seed. We quantize positions to avoid bit-level float noise.
  auto q = [](double v) -> std::int64_t {
    // 1/16 mkm (62,500 km) grid: stable but still responsive.
    return static_cast<std::int64_t>(std::llround(v * 16.0));
  };

  std::uint64_t seed = 0x3D2C0A8F1B6E5D9BULL;
  seed ^= static_cast<std::uint64_t>(system_id) * 0x9E3779B97F4A7C15ULL;
  seed ^= static_cast<std::uint64_t>(q(from_mkm.x)) * 0xBF58476D1CE4E5B9ULL;
  seed ^= static_cast<std::uint64_t>(q(from_mkm.y)) * 0x94D049BB133111EBULL;
  seed ^= static_cast<std::uint64_t>(q(to_mkm.x)) * 0xD6E8FEB86659FD93ULL;
  seed ^= static_cast<std::uint64_t>(q(to_mkm.y)) * 0x2545F4914F6CDD1DULL;
  seed ^= procgen_obscure::splitmix64(extra_seed ^ 0x8E1D9C0B7A53F241ULL);
  seed = procgen_obscure::splitmix64(seed);

  auto rand01 = [&](std::uint64_t salt) -> double {
    return procgen_obscure::u01_from_u64(procgen_obscure::splitmix64(seed ^ salt));
  };

  // Config knobs (clamped to sane ranges).
  const double iso = std::clamp(cfg_.terrain_nav_iso_speed, 0.05, 1.0);
  const double step_scale = std::clamp(cfg_.terrain_nav_sdf_step_scale, 0.05, 1.0);
  const double min_step = std::max(1e-3, cfg_.terrain_nav_min_step_mkm);
  const double max_step = std::max(min_step, cfg_.terrain_nav_max_step_mkm);
  const double eps = std::max(1e-3, cfg_.terrain_nav_grad_epsilon_mkm);
  const int max_steps = std::clamp(cfg_.terrain_nav_max_steps, 4, 512);
  const double jitter = std::clamp(cfg_.terrain_nav_sample_jitter, 0.0, 0.49);

  // Adaptive ray-march integration of movement *time* cost along the segment.
  //
  // We treat the scalar field f(p) = speed_at(p) as an implicit surface via
  // h(p) = f(p) - iso and use a signed-distance *estimate* sd ~= h/|grad h| to
  // set a conservative step size (sphere-tracing style). This is not a true SDF
  // but works well as a heuristic around sharp fronts.
  double sum_ds_over_m = 0.0;
  double s = 0.0;

  // Small deterministic start offset reduces banding when many queries share similar geometry.
  {
    const double off = (rand01(0xD15EA5EULL) - 0.5) * min_step;
    if (std::isfinite(off)) s = std::clamp(off, 0.0, 0.75 * min_step);
  }

  for (int step = 0; step < max_steps && s < L - 1e-9; ++step) {
    const Vec2 p = from_mkm + dir * s;
    const double f = speed_at(p);
    const double h = f - iso;

    // Central-difference gradient of h.
    const double fxp = speed_at(Vec2{p.x + eps, p.y});
    const double fxm = speed_at(Vec2{p.x - eps, p.y});
    const double fyp = speed_at(Vec2{p.x, p.y + eps});
    const double fym = speed_at(Vec2{p.x, p.y - eps});
    const double gx = (fxp - fxm) / (2.0 * eps);
    const double gy = (fyp - fym) / (2.0 * eps);
    const double grad = std::sqrt(gx * gx + gy * gy);

    double dist_est = max_step;
    if (std::isfinite(grad) && grad > 1e-9) {
      dist_est = std::abs(h) / grad;
    }

    double ds = std::clamp(dist_est * step_scale, min_step, max_step);
    if (s + ds > L) ds = L - s;

    // Stratified sample within the step (deterministic jitter).
    double u = (rand01(0xC0FFEEULL + static_cast<std::uint64_t>(step)) - 0.5) * 2.0;
    u *= jitter;
    double t = std::clamp(0.5 + u, 0.0, 1.0);
    const Vec2 ps = from_mkm + dir * (s + ds * t);

    const double m = std::max(0.05, speed_at(ps));
    sum_ds_over_m += ds / m;

    s += ds;
  }

  if (!std::isfinite(sum_ds_over_m) || sum_ds_over_m <= 1e-12) {
    return L / baseline;
  }

  return sum_ds_over_m;
}

double Simulation::system_movement_speed_multiplier_at(Id system_id, const Vec2& pos_mkm) const {
  const auto* sys = find_ptr(state_.systems, system_id);
  if (!sys) return 1.0;

  const double neb = std::clamp(system_nebula_density_at(system_id, pos_mkm), 0.0, 1.0);
  const double storm = (cfg_.enable_nebula_storms ? system_storm_intensity_at(system_id, pos_mkm) : 0.0);

  double m = 1.0;

  if (cfg_.enable_nebula_drag) {
    const double drag = std::max(0.0, cfg_.nebula_drag_speed_penalty_at_max_density);
    m *= std::clamp(1.0 - drag * neb, 0.05, 1.0);
  }

  if (cfg_.enable_nebula_storms) {
    const double pen = std::max(0.0, cfg_.nebula_storm_speed_penalty);
    m *= std::clamp(1.0 - pen * storm, 0.05, 1.0);
  }

  return std::clamp(m, 0.05, 1.0);
}


void Simulation::ensure_piracy_presence_cache_current() const {
  const std::int64_t day = state_.date.days_since_epoch();
  const int hour = std::clamp(state_.hour_of_day, 0, 23);

  if (piracy_presence_cache_valid_ && piracy_presence_cache_day_ == day && piracy_presence_cache_hour_ == hour &&
      piracy_presence_cache_state_generation_ == state_generation_ &&
      piracy_presence_cache_content_generation_ == content_generation_) {
    return;
  }

  piracy_presence_cache_valid_ = true;
  piracy_presence_cache_day_ = day;
  piracy_presence_cache_hour_ = hour;
  piracy_presence_cache_state_generation_ = state_generation_;
  piracy_presence_cache_content_generation_ = content_generation_;

  piracy_presence_cache_.clear();
  piracy_presence_cache_.reserve(state_.systems.size() * 2 + 8);

  const double scale = std::max(1e-6, cfg_.pirate_suppression_power_scale);

  auto pirate_threat_for_design = [&](const ShipDesign& d, bool is_hideout) -> double {
    double weapons = std::max(0.0, d.weapon_damage) +
                     std::max(0.0, d.missile_damage) +
                     0.50 * std::max(0.0, d.point_defense_damage);

    double durability = 0.05 * (std::max(0.0, d.max_hp) + std::max(0.0, d.max_shields));

    double sensors = 0.02 * std::max(0.0, d.sensor_range_mkm);

    double threat = weapons + durability + sensors;
    if (is_hideout) threat *= 1.5;
    return std::max(0.0, threat);
  };

  for (const auto& [sid, ship] : state_.ships) {
    (void)sid;
    if (ship.hp <= 0.0) continue;
    if (ship.system_id == kInvalidId) continue;

    const auto* fac = find_ptr(state_.factions, ship.faction_id);
    if (!fac || fac->control != FactionControl::AI_Pirate) continue;

    const auto* d = find_design(ship.design_id);
    if (!d) continue;

    const bool is_hideout = (ship.design_id == "pirate_hideout");
    const double threat = pirate_threat_for_design(*d, is_hideout);
    if (threat <= 1e-12) continue;

    auto& info = piracy_presence_cache_[ship.system_id];
    info.pirate_threat += threat;
    if (is_hideout) {
      ++info.pirate_hideouts;
    } else {
      ++info.pirate_ships;
    }
  }

  for (auto& [sys_id, info] : piracy_presence_cache_) {
    (void)sys_id;
    const double t = std::max(0.0, info.pirate_threat);
    // Convert threat into a [0,1] disruption risk. We reuse the suppression
    // power scale to keep tuning intuitive.
    info.presence_risk = 1.0 - std::exp(-t / scale);
    info.presence_risk = std::clamp(info.presence_risk, 0.0, 1.0);
  }
}


void Simulation::ensure_blockade_cache_current() const {
  if (!cfg_.enable_blockades) {
    invalidate_blockade_cache();
    return;
  }

  const std::int64_t day = state_.date.days_since_epoch();
  const int hour = std::clamp(state_.hour_of_day, 0, 23);

  if (blockade_cache_valid_ && blockade_cache_day_ == day && blockade_cache_hour_ == hour &&
      blockade_cache_state_generation_ == state_generation_ &&
      blockade_cache_content_generation_ == content_generation_) {
    return;
  }

  blockade_cache_valid_ = true;
  blockade_cache_day_ = day;
  blockade_cache_hour_ = hour;
  blockade_cache_state_generation_ = state_generation_;
  blockade_cache_content_generation_ = content_generation_;

  blockade_cache_.clear();
  blockade_cache_.reserve(state_.colonies.size() * 2 + 8);

  const double radius_mkm = std::max(0.0, cfg_.blockade_radius_mkm);

  auto ship_power = [&](const Ship& sh) -> double {
    const ShipDesign* d = find_design(sh.design_id);
    if (!d) return 0.0;
    const double w = std::max(0.0, d->weapon_damage) +
                     std::max(0.0, d->missile_damage) +
                     0.5 * std::max(0.0, d->point_defense_damage);
    if (!(w > 1e-9)) return 0.0;

    // Mirror the piracy-suppression heuristic: weapons + a bit of durability/sensors.
    const double dur = 0.05 * std::max(0.0, sh.hp + sh.shields);
    const double sen = 0.25 * std::max(0.0, d->sensor_range_mkm);
    return std::max(0.0, w + dur + sen);
  };

  const double static_mult = 1.0;
  const double base_resist = std::max(0.0, cfg_.blockade_base_resistance_power);
  const double max_penalty = std::clamp(cfg_.blockade_max_output_penalty, 0.0, 1.0);

  for (const auto& [cid, col] : state_.colonies) {
    (void)cid;
    BlockadeStatus bs;
    bs.colony_id = col.id;

    const Body* body = find_ptr(state_.bodies, col.body_id);
    if (!body) {
      bs.pressure = 0.0;
      bs.output_multiplier = 1.0;
      blockade_cache_.emplace(col.id, bs);
      continue;
    }
    const Id sys_id = body->system_id;
    if (sys_id == kInvalidId) {
      bs.pressure = 0.0;
      bs.output_multiplier = 1.0;
      blockade_cache_.emplace(col.id, bs);
      continue;
    }

    const auto* sys = find_ptr(state_.systems, sys_id);
    if (!sys) {
      bs.pressure = 0.0;
      bs.output_multiplier = 1.0;
      blockade_cache_.emplace(col.id, bs);
      continue;
    }

    const Vec2 anchor = body->position_mkm;

    // Nearby ship presence.
    for (Id sid : sys->ships) {
      const Ship* sh = find_ptr(state_.ships, sid);
      if (!sh) continue;
      if (sh->hp <= 1e-9) continue;
      if (sh->system_id != sys_id) continue;
      if (radius_mkm > 0.0) {
        const double d = (sh->position_mkm - anchor).length();
        if (d > radius_mkm + 1e-9) continue;
      }
      if (sh->faction_id == kInvalidId) continue;

      const double p = ship_power(*sh);
      if (!(p > 1e-9)) continue;

      if (sh->faction_id == col.faction_id || are_factions_trade_partners(col.faction_id, sh->faction_id)) {
        bs.defender_power += p;
        bs.defender_ships += 1;
      } else if (are_factions_hostile(sh->faction_id, col.faction_id) ||
                 are_factions_hostile(col.faction_id, sh->faction_id)) {
        bs.hostile_power += p;
        bs.hostile_ships += 1;
      }
    }

    // Static defensive weapons.
    double static_power = 0.0;
    for (const auto& [inst_id, count] : col.installations) {
      if (count <= 0) continue;
      auto it = content_.installations.find(inst_id);
      if (it == content_.installations.end()) continue;
      const auto& def = it->second;
      const double w = std::max(0.0, def.weapon_damage) + 0.5 * std::max(0.0, def.point_defense_damage);
      if (w > 1e-9) static_power += w * static_cast<double>(count);
    }
    bs.defender_power += static_power * static_mult;

    const double denom = bs.hostile_power + bs.defender_power + base_resist;
    if (denom > 1e-9) {
      bs.pressure = std::clamp(bs.hostile_power / denom, 0.0, 1.0);
    } else {
      bs.pressure = 0.0;
    }

    // Convert pressure into an output multiplier.
    bs.output_multiplier = std::clamp(1.0 - bs.pressure * max_penalty, 1.0 - max_penalty, 1.0);

    blockade_cache_.emplace(col.id, bs);
  }
}

void Simulation::invalidate_blockade_cache() const {
  blockade_cache_valid_ = false;
  blockade_cache_.clear();
  blockade_cache_day_ = state_.date.days_since_epoch();
  blockade_cache_hour_ = std::clamp(state_.hour_of_day, 0, 23);
  blockade_cache_state_generation_ = state_generation_;
  blockade_cache_content_generation_ = content_generation_;
}


void Simulation::ensure_civilian_shipping_loss_cache_current() const {
  // The shipping-loss metric is derived from wrecks; if wrecks are disabled or
  // memory is disabled, treat it as inactive.
  if (!cfg_.enable_wrecks || cfg_.civilian_shipping_loss_memory_days <= 0) {
    invalidate_civilian_shipping_loss_cache();
    return;
  }

  const std::int64_t day = state_.date.days_since_epoch();

  if (civilian_shipping_loss_cache_valid_ && civilian_shipping_loss_cache_day_ == day &&
      civilian_shipping_loss_cache_state_generation_ == state_generation_ &&
      civilian_shipping_loss_cache_content_generation_ == content_generation_) {
    return;
  }

  civilian_shipping_loss_cache_valid_ = true;
  civilian_shipping_loss_cache_day_ = day;
  civilian_shipping_loss_cache_state_generation_ = state_generation_;
  civilian_shipping_loss_cache_content_generation_ = content_generation_;

  civilian_shipping_loss_cache_.clear();
  civilian_shipping_loss_cache_.reserve(state_.systems.size() * 2 + 8);

  // Locate the Merchant Guild faction id (neutral civilians).
  Id merchant_fid = kInvalidId;
  for (const auto& [fid, fac] : state_.factions) {
    if (fac.control == FactionControl::AI_Passive && fac.name == "Merchant Guild") {
      merchant_fid = fid;
      break;
    }
  }

  if (merchant_fid == kInvalidId) return;
  if (state_.wrecks.empty()) return;

  const double memory_days = std::max(1.0, static_cast<double>(cfg_.civilian_shipping_loss_memory_days));
  const double scale = std::max(1e-6, cfg_.civilian_shipping_loss_pressure_scale);

  // Aggregate raw loss "scores" per system (sum of decayed wreck weights).
  std::unordered_map<Id, double> score;
  std::unordered_map<Id, int> count;
  score.reserve(state_.systems.size() * 2 + 8);
  count.reserve(state_.systems.size() * 2 + 8);

  const std::int64_t now = day;
  for (const auto& [wid, w] : state_.wrecks) {
    (void)wid;
    if (w.system_id == kInvalidId) continue;
    if (w.kind != WreckKind::Ship) continue;
    if (w.source_faction_id != merchant_fid) continue;

    const std::int64_t created = w.created_day;
    if (created <= 0) continue;

    const double age = static_cast<double>(std::max<std::int64_t>(0, now - created));
    if (age > memory_days + 1e-9) continue;

    // Linear decay: 1 at age=0, 0 at age=memory_days.
    const double wt = std::clamp(1.0 - age / memory_days, 0.0, 1.0);
    if (wt <= 1e-9) continue;

    score[w.system_id] += wt;
    count[w.system_id] += 1;
  }

  for (const auto& [sys_id, sc] : score) {
    CivilianShippingLossStatus st;
    st.system_id = sys_id;
    st.score = std::max(0.0, sc);
    st.recent_wrecks = 0;
    if (auto itc = count.find(sys_id); itc != count.end()) st.recent_wrecks = itc->second;

    // Map score -> pressure in [0,1].
    st.pressure = 1.0 - std::exp(-st.score / scale);
    st.pressure = std::clamp(st.pressure, 0.0, 1.0);

    civilian_shipping_loss_cache_[sys_id] = st;
  }
}

void Simulation::invalidate_civilian_shipping_loss_cache() const {
  civilian_shipping_loss_cache_valid_ = false;
  civilian_shipping_loss_cache_.clear();
  civilian_shipping_loss_cache_day_ = state_.date.days_since_epoch();
  civilian_shipping_loss_cache_state_generation_ = state_generation_;
  civilian_shipping_loss_cache_content_generation_ = content_generation_;
}

CivilianShippingLossStatus Simulation::civilian_shipping_loss_status_for_system(Id system_id) const {
  CivilianShippingLossStatus st;
  st.system_id = system_id;
  if (system_id == kInvalidId) return st;
  if (!cfg_.enable_wrecks || cfg_.civilian_shipping_loss_memory_days <= 0) return st;

  ensure_civilian_shipping_loss_cache_current();
  auto it = civilian_shipping_loss_cache_.find(system_id);
  if (it != civilian_shipping_loss_cache_.end()) return it->second;

  return st;
}

double Simulation::civilian_shipping_loss_pressure_for_system(Id system_id) const {
  return std::clamp(civilian_shipping_loss_status_for_system(system_id).pressure, 0.0, 1.0);
}

CivilianTradeActivityStatus Simulation::civilian_trade_activity_status_for_system(Id system_id) const {
  CivilianTradeActivityStatus st;
  st.system_id = system_id;
  if (system_id == kInvalidId) return st;

  const auto* sys = find_ptr(state_.systems, system_id);
  if (!sys) return st;

  st.score = std::max(0.0, sys->civilian_trade_activity_score);

  const double scale = std::max(1e-6, cfg_.civilian_trade_activity_score_scale_tons);
  st.factor = 1.0 - std::exp(-st.score / scale);
  st.factor = std::clamp(st.factor, 0.0, 1.0);
  return st;
}

double Simulation::civilian_trade_activity_factor_for_system(Id system_id) const {
  return std::clamp(civilian_trade_activity_status_for_system(system_id).factor, 0.0, 1.0);
}

double Simulation::ambient_piracy_risk_for_system(Id system_id) const {
  const auto* sys = find_ptr(state_.systems, system_id);
  if (!sys) return 0.0;

  double risk = std::clamp(cfg_.pirate_raid_default_system_risk, 0.0, 1.0);
  double suppression = 0.0;

  if (sys->region_id != kInvalidId) {
    if (const auto* reg = find_ptr(state_.regions, sys->region_id)) {
      risk = std::clamp(reg->pirate_risk, 0.0, 1.0);
      if (cfg_.enable_pirate_suppression) {
        suppression = std::clamp(reg->pirate_suppression, 0.0, 1.0);
      }
    }
  }

  risk *= (1.0 - suppression);
  return std::clamp(risk, 0.0, 1.0);
}

double Simulation::pirate_presence_risk_for_system(Id system_id) const {
  if (system_id == kInvalidId) return 0.0;
  ensure_piracy_presence_cache_current();
  auto it = piracy_presence_cache_.find(system_id);
  if (it == piracy_presence_cache_.end()) return 0.0;
  return std::clamp(it->second.presence_risk, 0.0, 1.0);
}

double Simulation::piracy_risk_for_system(Id system_id) const {
  const double ambient = ambient_piracy_risk_for_system(system_id);
  const double presence = pirate_presence_risk_for_system(system_id);
  // Combine as a probabilistic union so either component can dominate without
  // exceeding 1.
  const double combined = 1.0 - (1.0 - ambient) * (1.0 - presence);
  return std::clamp(combined, 0.0, 1.0);
}


BlockadeStatus Simulation::blockade_status_for_colony(Id colony_id) const {
  if (colony_id == kInvalidId) return BlockadeStatus{};
  if (!cfg_.enable_blockades) {
    BlockadeStatus bs;
    bs.colony_id = colony_id;
    bs.pressure = 0.0;
    bs.output_multiplier = 1.0;
    return bs;
  }

  ensure_blockade_cache_current();
  auto it = blockade_cache_.find(colony_id);
  if (it != blockade_cache_.end()) return it->second;

  BlockadeStatus bs;
  bs.colony_id = colony_id;
  bs.pressure = 0.0;
  bs.output_multiplier = 1.0;
  return bs;
}

double Simulation::blockade_output_multiplier_for_colony(Id colony_id) const {
  return std::clamp(blockade_status_for_colony(colony_id).output_multiplier, 0.0, 1.0);
}


void Simulation::ensure_trade_prosperity_cache_current() const {
  if (!cfg_.enable_trade_prosperity) {
    invalidate_trade_prosperity_cache();
    return;
  }

  const std::int64_t day = state_.date.days_since_epoch();

  if (trade_prosperity_cache_valid_ && trade_prosperity_cache_day_ == day &&
      trade_prosperity_cache_state_generation_ == state_generation_ &&
      trade_prosperity_cache_content_generation_ == content_generation_) {
    return;
  }

  trade_prosperity_system_cache_.clear();
  trade_prosperity_system_cache_.reserve(state_.systems.size() * 2 + 8);

  TradeNetworkOptions opt;
  opt.max_lanes = 0;
  // Keep other defaults: include_uncolonized_markets + include_colony_contributions.
  const TradeNetwork tn = compute_trade_network(*this, opt);

  for (const auto& node : tn.nodes) {
    TradeProsperitySystemInfo info;
    info.market_size = std::max(0.0, node.market_size);
    info.hub_score = std::clamp(node.hub_score, 0.0, 1.0);

    // Optional feedback from real civilian trade volume.
    // This makes Trade Prosperity respond to actual Merchant Guild freight
    // transfers, not just the static "theoretical" network.
    if (cfg_.enable_civilian_trade_activity_prosperity) {
      const CivilianTradeActivityStatus act = civilian_trade_activity_status_for_system(node.system_id);
      const double f = std::clamp(act.factor, 0.0, 1.0);
      if (f > 1e-9) {
        const double hub_cap = std::max(0.0, cfg_.civilian_trade_activity_hub_score_bonus_cap);
        const double market_cap = std::max(0.0, cfg_.civilian_trade_activity_market_size_bonus_cap);
        info.hub_score = std::clamp(info.hub_score + hub_cap * f, 0.0, 1.0);
        info.market_size = std::max(0.0, info.market_size * (1.0 + market_cap * f));
      }
    }
    trade_prosperity_system_cache_[node.system_id] = info;
  }

  trade_prosperity_cache_valid_ = true;
  trade_prosperity_cache_day_ = day;
  trade_prosperity_cache_state_generation_ = state_generation_;
  trade_prosperity_cache_content_generation_ = content_generation_;
}

void Simulation::invalidate_trade_prosperity_cache() const {
  trade_prosperity_cache_valid_ = false;
  trade_prosperity_system_cache_.clear();
  trade_prosperity_cache_day_ = state_.date.days_since_epoch();
  trade_prosperity_cache_state_generation_ = state_generation_;
  trade_prosperity_cache_content_generation_ = content_generation_;
}

TradeProsperityStatus Simulation::trade_prosperity_status_for_colony(Id colony_id) const {
  TradeProsperityStatus st;
  st.colony_id = colony_id;
  st.output_multiplier = 1.0;
  st.output_bonus = 0.0;

  if (colony_id == kInvalidId) return st;
  if (!cfg_.enable_trade_prosperity) return st;

  const Colony* col = find_ptr(state_.colonies, colony_id);
  if (!col) return st;

  Id sys_id = kInvalidId;
  if (const Body* b = find_ptr(state_.bodies, col->body_id)) {
    sys_id = b->system_id;
  }

  ensure_trade_prosperity_cache_current();

  if (sys_id != kInvalidId) {
    auto it = trade_prosperity_system_cache_.find(sys_id);
    if (it != trade_prosperity_system_cache_.end()) {
      st.market_size = std::max(0.0, it->second.market_size);
      st.hub_score = std::clamp(it->second.hub_score, 0.0, 1.0);
    }
  }

  // Treaty-driven market access boost (diplomacy).
  //
  // This is intentionally a lightweight proxy for the idea that more open trade
  // relationships give your colonies access to a larger market.
  st.trade_partner_count = 0;
  st.treaty_market_boost = 1.0;
  st.effective_market_size = st.market_size;

  if (col->faction_id != kInvalidId) {
    int partners = 0;
    for (const auto& [other_id, _] : state_.factions) {
      if (other_id == col->faction_id) continue;
      if (are_factions_trade_partners(col->faction_id, other_id)) {
        ++partners;
      }
    }
    st.trade_partner_count = partners;

    if (cfg_.enable_trade_prosperity_treaty_market_boost) {
      const double per = std::max(0.0, cfg_.trade_prosperity_treaty_market_boost_per_trade_partner);
      const double maxb = std::max(0.0, cfg_.trade_prosperity_treaty_market_boost_max);
      const double boost = std::clamp(per * static_cast<double>(partners), 0.0, maxb);
      st.treaty_market_boost = 1.0 + boost;
      st.effective_market_size = st.market_size * st.treaty_market_boost;
    }
  }

  const double half_market = std::max(1e-9, cfg_.trade_prosperity_market_size_half_bonus);
  const double eff_market = std::max(0.0, st.effective_market_size);
  st.market_factor = std::clamp(eff_market / (eff_market + half_market), 0.0, 1.0);

  const double pop = std::max(0.0, col->population_millions);
  const double half_pop = std::max(1e-9, cfg_.trade_prosperity_pop_half_bonus_millions);
  st.pop_factor = std::clamp(pop / (pop + half_pop), 0.0, 1.0);

  const double hub_infl = std::clamp(cfg_.trade_prosperity_hub_influence, 0.0, 1.0);
  const double hub_term = std::clamp((1.0 - hub_infl) + hub_infl * st.hub_score, 0.0, 1.0);

  const double max_bonus = std::max(0.0, cfg_.trade_prosperity_max_output_bonus);
  double base_bonus = max_bonus * st.market_factor * st.pop_factor * hub_term;
  base_bonus = std::clamp(base_bonus, 0.0, max_bonus);

  // Piracy disruption (ambient risk + active pirate presence).
  st.piracy_risk = piracy_risk_for_system(sys_id);


  // Blockade disruption (reuses cached blockade pressure when enabled).
  st.blockade_pressure = std::clamp(blockade_status_for_colony(colony_id).pressure, 0.0, 1.0);

  // Shipping-loss disruption (recent civilian losses can depress trade for a
  // while even if pirates leave).
  st.shipping_loss_pressure = std::clamp(civilian_shipping_loss_pressure_for_system(sys_id), 0.0, 1.0);

  const double piracy_w = std::clamp(cfg_.trade_prosperity_piracy_risk_penalty, 0.0, 1.0);
  const double blockade_w = std::clamp(cfg_.trade_prosperity_blockade_pressure_penalty, 0.0, 1.0);
  const double shipping_w = std::clamp(cfg_.trade_prosperity_shipping_loss_penalty, 0.0, 1.0);

  double penalty = piracy_w * st.piracy_risk +
                   blockade_w * st.blockade_pressure +
                   shipping_w * st.shipping_loss_pressure;
  penalty = std::clamp(penalty, 0.0, 1.0);

  const double bonus = std::clamp(base_bonus * (1.0 - penalty), 0.0, max_bonus);
  st.output_bonus = bonus;
  st.output_multiplier = 1.0 + bonus;
  return st;
}

double Simulation::trade_prosperity_output_multiplier_for_colony(Id colony_id) const {
  return std::max(0.0, trade_prosperity_status_for_colony(colony_id).output_multiplier);
}

namespace {

struct ColonyConditionDef {
  const char* id{nullptr};
  const char* name{nullptr};
  const char* description{nullptr};

  bool positive{false};
  bool resolvable{false};

  // Net stability delta contributed by this condition at severity=1.
  // (Added to ColonyStabilityStatus::stability before clamping.)
  double stability_delta{0.0};

  // Nominal output multipliers at severity=1.
  ColonyConditionMultipliers mult;

  // Baseline resolve costs (minerals). 0 => not used.
  double resolve_duranium{0.0};
  double resolve_tritanium{0.0};
  double resolve_boronide{0.0};
  double resolve_corbomite{0.0};
};

const ColonyConditionDef* colony_condition_def(const std::string& id) {
  // NOTE: ids are intentionally stable strings to keep saves forward-compatible.
  static const ColonyConditionDef kDefs[] = {
      // --- Negative ---
      {"industrial_accident",
       "Industrial Accident",
       "A serious incident has disrupted heavy industry and construction until repairs and safety audits are completed.",
       /*positive=*/false,
       /*resolvable=*/true,
       /*stability_delta=*/-0.15,
       /*mult=*/{/*mining=*/0.98,
                /*industry=*/0.75,
                /*research=*/0.98,
                /*construction=*/0.80,
                /*shipyard=*/0.80,
                /*terraforming=*/0.95,
                /*troop_training=*/0.95,
                /*pop_growth=*/0.99},
       /*resolve_duranium=*/250.0,
       /*resolve_tritanium=*/120.0,
       /*resolve_boronide=*/0.0,
       /*resolve_corbomite=*/0.0},

      {"labor_strike",
       "Labor Strike",
       "Organized labor action is reducing throughput as negotiations continue.",
       /*positive=*/false,
       /*resolvable=*/true,
       /*stability_delta=*/-0.20,
       /*mult=*/{/*mining=*/0.80,
                /*industry=*/0.80,
                /*research=*/0.95,
                /*construction=*/0.85,
                /*shipyard=*/0.85,
                /*terraforming=*/0.95,
                /*troop_training=*/0.90,
                /*pop_growth=*/0.99},
       /*resolve_duranium=*/180.0,
       /*resolve_tritanium=*/0.0,
       /*resolve_bboronide=*/0.0,
       /*resolve_corbomite=*/60.0},

      {"disease_outbreak",
       "Disease Outbreak",
       "A fast-spreading pathogen is reducing productivity while medical services mobilize.",
       /*positive=*/false,
       /*resolvable=*/true,
       /*stability_delta=*/-0.25,
       /*mult=*/{/*mining=*/0.95,
                /*industry=*/0.92,
                /*research=*/0.85,
                /*construction=*/0.95,
                /*shipyard=*/0.95,
                /*terraforming=*/0.98,
                /*troop_training=*/0.85,
                /*pop_growth=*/0.70},
       /*resolve_duranium=*/200.0,
       /*resolve_tritanium=*/0.0,
       /*resolve_boronide=*/120.0,
       /*resolve_corbomite=*/0.0},

      // --- Positive ---
      {"cultural_festival",
       "Cultural Festival",
       "A major celebration is boosting morale and civic engagement across the colony.",
       /*positive=*/true,
       /*resolvable=*/false,
       /*stability_delta=*/0.10,
       /*mult=*/{/*mining=*/1.00,
                /*industry=*/0.98,
                /*research=*/1.10,
                /*construction=*/1.00,
                /*shipyard=*/1.00,
                /*terraforming=*/1.00,
                /*troop_training=*/1.00,
                /*pop_growth=*/1.08},
       /*resolve_duranium=*/0.0,
       /*resolve_tritanium=*/0.0,
       /*resolve_boronide=*/0.0,
       /*resolve_corbomite=*/0.0},

      {"engineering_breakthrough",
       "Engineering Breakthrough",
       "A wave of process innovations is accelerating construction and shipyard throughput.",
       /*positive=*/true,
       /*resolvable=*/false,
       /*stability_delta=*/0.12,
       /*mult=*/{/*mining=*/1.00,
                /*industry=*/1.05,
                /*research=*/1.00,
                /*construction=*/1.25,
                /*shipyard=*/1.20,
                /*terraforming=*/1.00,
                /*troop_training=*/1.00,
                /*pop_growth=*/1.00},
       /*resolve_duranium=*/0.0,
       /*resolve_tritanium=*/0.0,
       /*resolve_boronide=*/0.0,
       /*resolve_corbomite=*/0.0},

      {"terraforming_breakthrough",
       "Terraforming Breakthrough",
       "Unexpected synergies in environmental engineering are improving terraforming efficiency.",
       /*positive=*/true,
       /*resolvable=*/false,
       /*stability_delta=*/0.08,
       /*mult=*/{/*mining=*/1.00,
                /*industry=*/1.00,
                /*research=*/1.00,
                /*construction=*/1.00,
                /*shipyard=*/1.00,
                /*terraforming=*/1.40,
                /*troop_training=*/1.00,
                /*pop_growth=*/1.00},
       /*resolve_duranium=*/0.0,
       /*resolve_tritanium=*/0.0,
       /*resolve_boronide=*/0.0,
       /*resolve_corbomite=*/0.0},
  };

  for (const auto& d : kDefs) {
    if (d.id && id == d.id) return &d;
  }
  return nullptr;
}

double clamp_condition_mult(double v) {
  if (!std::isfinite(v)) return 1.0;
  return std::clamp(v, 0.05, 5.0);
}

} // namespace

ColonyConditionMultipliers Simulation::colony_condition_multipliers_for_condition(const ColonyCondition& condition) const {
  ColonyConditionMultipliers out;
  if (!cfg_.enable_colony_conditions) return out;
  if (condition.id.empty()) return out;

  const auto* def = colony_condition_def(condition.id);
  if (!def) return out;

  const double sev = std::clamp(condition.severity, 0.0, 10.0);

  auto scale = [&](double base) -> double {
    base = std::max(1e-6, base);
    const double v = std::pow(base, sev);
    return clamp_condition_mult(v);
  };

  out.mining = scale(def->mult.mining);
  out.industry = scale(def->mult.industry);
  out.research = scale(def->mult.research);
  out.construction = scale(def->mult.construction);
  out.shipyard = scale(def->mult.shipyard);
  out.terraforming = scale(def->mult.terraforming);
  out.troop_training = scale(def->mult.troop_training);
  out.pop_growth = scale(def->mult.pop_growth);

  return out;
}

ColonyConditionMultipliers Simulation::colony_condition_multipliers(const Colony& colony) const {
  ColonyConditionMultipliers total;
  if (!cfg_.enable_colony_conditions) return total;

  for (const auto& cond : colony.conditions) {
    if (cond.remaining_days <= 1e-9) continue;
    const ColonyConditionMultipliers m = colony_condition_multipliers_for_condition(cond);
    total.mining *= m.mining;
    total.industry *= m.industry;
    total.research *= m.research;
    total.construction *= m.construction;
    total.shipyard *= m.shipyard;
    total.terraforming *= m.terraforming;
    total.troop_training *= m.troop_training;
    total.pop_growth *= m.pop_growth;
  }

  total.mining = clamp_condition_mult(total.mining);
  total.industry = clamp_condition_mult(total.industry);
  total.research = clamp_condition_mult(total.research);
  total.construction = clamp_condition_mult(total.construction);
  total.shipyard = clamp_condition_mult(total.shipyard);
  total.terraforming = clamp_condition_mult(total.terraforming);
  total.troop_training = clamp_condition_mult(total.troop_training);
  total.pop_growth = clamp_condition_mult(total.pop_growth);

  return total;
}

std::string Simulation::colony_condition_display_name(const std::string& condition_id) const {
  if (condition_id.empty()) return {};
  if (const auto* def = colony_condition_def(condition_id)) {
    if (def->name) return def->name;
  }
  return condition_id;
}

std::string Simulation::colony_condition_description(const std::string& condition_id) const {
  if (condition_id.empty()) return {};
  if (const auto* def = colony_condition_def(condition_id)) {
    if (def->description) return def->description;
  }
  return {};
}

bool Simulation::colony_condition_is_positive(const std::string& condition_id) const {
  if (condition_id.empty()) return false;
  if (const auto* def = colony_condition_def(condition_id)) return def->positive;
  return false;
}

std::unordered_map<std::string, double> Simulation::colony_condition_resolve_cost(
    Id colony_id, const ColonyCondition& condition) const {
  std::unordered_map<std::string, double> out;

  if (!cfg_.enable_colony_conditions) return out;
  if (colony_id == kInvalidId) return out;
  if (condition.id.empty()) return out;
  if (condition.remaining_days <= 1e-9) return out;

  const auto* def = colony_condition_def(condition.id);
  if (!def || !def->resolvable) return out;

  const Colony* col = find_ptr(state_.colonies, colony_id);
  if (!col) return out;

  // Cost scales softly with population and severity.
  const double pop = std::max(0.0, col->population_millions);
  double pop_scale = 1.0 + std::sqrt(pop / 500.0);  // ~2x at 500M, ~3x at 2000M.
  pop_scale = std::clamp(pop_scale, 1.0, 5.0);

  const double sev_scale = std::clamp(condition.severity, 0.25, 3.0);
  const double scale = pop_scale * sev_scale;

  auto add = [&](const char* mineral, double base) {
    if (!mineral) return;
    if (!(base > 0.0)) return;
    const double v = std::max(0.0, base * scale);
    if (v <= 1e-6) return;
    out[mineral] += v;
  };

  add("Duranium", def->resolve_duranium);
  add("Tritanium", def->resolve_tritanium);
  add("Boronide", def->resolve_boronide);
  add("Corbomite", def->resolve_corbomite);

  return out;
}

bool Simulation::resolve_colony_condition(Id colony_id, const std::string& condition_id, std::string* error) {
  if (error) error->clear();

  if (!cfg_.enable_colony_conditions) {
    if (error) *error = "Colony conditions are disabled in simulation config.";
    return false;
  }

  Colony* col = find_ptr(state_.colonies, colony_id);
  if (!col) {
    if (error) *error = "Invalid colony.";
    return false;
  }

  auto it = std::find_if(col->conditions.begin(), col->conditions.end(),
                         [&](const ColonyCondition& c) { return c.id == condition_id && c.remaining_days > 1e-9; });
  if (it == col->conditions.end()) {
    if (error) *error = "Condition not found.";
    return false;
  }

  const auto cost = colony_condition_resolve_cost(colony_id, *it);
  if (cost.empty()) {
    if (error) *error = "This condition cannot be resolved manually.";
    return false;
  }

  auto have = [&](const std::string& mineral) -> double {
    auto mit = col->minerals.find(mineral);
    if (mit == col->minerals.end()) return 0.0;
    return std::max(0.0, mit->second);
  };

  // Check affordability.
  for (const auto& [mineral, amt] : cost) {
    if (amt <= 1e-9) continue;
    if (have(mineral) + 1e-9 < amt) {
      if (error) *error = "Insufficient minerals to resolve condition.";
      return false;
    }
  }

  // Deduct cost.
  for (const auto& [mineral, amt] : cost) {
    if (amt <= 1e-9) continue;
    col->minerals[mineral] = have(mineral) - amt;
    if (col->minerals[mineral] <= 1e-9) col->minerals.erase(mineral);
  }

  const std::string name = colony_condition_display_name(it->id);
  col->conditions.erase(it);

  EventContext ctx;
  ctx.faction_id = col->faction_id;
  ctx.colony_id = colony_id;
  if (const auto* b = find_ptr(state_.bodies, col->body_id)) ctx.system_id = b->system_id;

  push_event(EventLevel::Info, EventCategory::General,
             "Resolved condition at " + col->name + ": " + name + ".", ctx);
  return true;
}

ColonyStabilityStatus Simulation::colony_stability_status_for_colony(Id colony_id) const {
  ColonyStabilityStatus st;
  st.colony_id = colony_id;
  st.stability = 1.0;

  if (colony_id == kInvalidId) return st;

  const Colony* col = find_ptr(state_.colonies, colony_id);
  if (!col) return st;

  return colony_stability_status_for_colony(*col);
}

ColonyStabilityStatus Simulation::colony_stability_status_for_colony(const Colony& col) const {
  ColonyStabilityStatus st;
  st.colony_id = col.id;
  st.stability = 1.0;

  if (col.id == kInvalidId) return st;

  st.habitability = std::clamp(body_habitability_for_faction(col.body_id, col.faction_id), 0.0, 1.0);

  const double pop = std::max(0.0, col.population_millions);
  if (pop > 1e-9) {
    const double required = std::max(0.0, required_habitation_capacity_millions(col));
    const double available = std::max(0.0, habitation_capacity_millions(col));
    const double shortfall = std::max(0.0, required - available);
    st.habitation_shortfall_frac = std::clamp(shortfall / pop, 0.0, 1.0);
  } else {
    st.habitation_shortfall_frac = 0.0;
  }

  const TradeProsperityStatus tp = trade_prosperity_status_for_colony(col.id);
  st.trade_bonus = std::max(0.0, tp.output_bonus);
  st.piracy_risk = std::clamp(tp.piracy_risk, 0.0, 1.0);
  st.blockade_pressure = std::clamp(tp.blockade_pressure, 0.0, 1.0);
  st.shipping_loss_pressure = std::clamp(tp.shipping_loss_pressure, 0.0, 1.0);

  // Conditions stability delta (additive).
  st.condition_delta = 0.0;
  if (cfg_.enable_colony_conditions) {
    for (const auto& cond : col.conditions) {
      if (cond.remaining_days <= 1e-9) continue;
      const auto* def = colony_condition_def(cond.id);
      if (!def) continue;
      const double sev = std::clamp(cond.severity, 0.0, 10.0);
      st.condition_delta += def->stability_delta * sev;
    }
    st.condition_delta = std::clamp(st.condition_delta, -1.0, 1.0);
  }

  // Stability model:
  // - baseline starts reasonably high so a "normal" mature colony is stable
  //   even before trade/hub effects are established.
  double stability = 0.75;

  stability += 0.15 * st.habitability;
  stability -= 0.25 * st.habitation_shortfall_frac;

  // Trade bonus has a small positive effect; disruption (security/blockade) has a larger negative effect.
  stability += 0.20 * std::clamp(st.trade_bonus, 0.0, 1.0);

  // Combine current piracy risk with a short memory of civilian losses. This
  // models a "confidence" effect: even if pirates are no longer present, a
  // streak of recent merchant losses can still depress stability.
  const double security_risk = 1.0 - (1.0 - st.piracy_risk) * (1.0 - st.shipping_loss_pressure);
  stability -= 0.10 * security_risk;
  stability -= 0.25 * st.blockade_pressure;

  stability += st.condition_delta;

  st.stability = std::clamp(stability, 0.0, 1.0);
  return st;
}


double Simulation::colony_stability_output_multiplier_for_colony(Id colony_id) const {
  if (!cfg_.enable_colony_stability_output_scaling) return 1.0;

  const Colony* col = find_ptr(state_.colonies, colony_id);
  if (!col) return 1.0;

  return colony_stability_output_multiplier_for_colony(*col);
}

double Simulation::colony_stability_output_multiplier_for_colony(const Colony& colony) const {
  if (!cfg_.enable_colony_stability_output_scaling) return 1.0;

  const double neutral = std::clamp(cfg_.colony_stability_neutral_threshold, 0.0, 1.0);
  const double min_mult = std::clamp(cfg_.colony_stability_min_output_multiplier, 0.0, 1.0);
  if (!(neutral > 1e-9)) return 1.0;

  const double st = std::clamp(colony_stability_status_for_colony(colony).stability, 0.0, 1.0);
  if (st >= neutral) return 1.0;

  const double t = std::clamp(st / neutral, 0.0, 1.0);
  return std::max(0.0, min_mult + (1.0 - min_mult) * t);
}





void Simulation::ensure_jump_route_cache_current() const {
  const std::int64_t day = state_.date.days_since_epoch();
  const int hour = state_.hour_of_day;
  if (!jump_route_cache_day_valid_ || jump_route_cache_day_ != day || jump_route_cache_hour_ != hour) {
    jump_route_cache_.clear();
    jump_route_cache_lru_.clear();
    jump_route_cache_day_ = day;
    jump_route_cache_hour_ = hour;
    jump_route_cache_day_valid_ = true;
  }
}

void Simulation::invalidate_jump_route_cache() const {
  jump_route_cache_.clear();
  jump_route_cache_lru_.clear();
  jump_route_cache_day_ = state_.date.days_since_epoch();
  jump_route_cache_hour_ = state_.hour_of_day;
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

  DiplomacyStatus base = DiplomacyStatus::Hostile;
  if (auto it = from->relations.find(to_faction_id); it != from->relations.end()) {
    base = it->second;
  }

  // Treaties are symmetric and stored with a normalized (min,max) faction pair.
  if (!state_.treaties.empty()) {
    Id a = from_faction_id;
    Id b = to_faction_id;
    normalize_faction_pair(a, b);

    bool force_friendly = false;
    bool at_least_neutral = false;
    for (const auto& [tid, t] : state_.treaties) {
      (void)tid;
      if (t.faction_a != a || t.faction_b != b) continue;
      switch (t.type) {
        case TreatyType::Alliance:
          force_friendly = true;
          break;
        case TreatyType::Ceasefire:
        case TreatyType::NonAggressionPact:
        case TreatyType::TradeAgreement:
        case TreatyType::ResearchAgreement:
          at_least_neutral = true;
          break;
      }
      if (force_friendly) break;
    }

    if (force_friendly) return DiplomacyStatus::Friendly;
    if (at_least_neutral && base == DiplomacyStatus::Hostile) return DiplomacyStatus::Neutral;
  }

  return base;
}

DiplomacyStatus Simulation::diplomatic_status_base(Id from_faction_id, Id to_faction_id) const {
  if (from_faction_id == kInvalidId || to_faction_id == kInvalidId) return DiplomacyStatus::Hostile;
  if (from_faction_id == to_faction_id) return DiplomacyStatus::Friendly;

  const auto* from = find_ptr(state_.factions, from_faction_id);
  if (!from) return DiplomacyStatus::Hostile;

  DiplomacyStatus base = DiplomacyStatus::Hostile;
  if (auto it = from->relations.find(to_faction_id); it != from->relations.end()) {
    base = it->second;
  }
  return base;
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

bool Simulation::are_factions_trade_partners(Id a_faction_id, Id b_faction_id) const {
  if (a_faction_id == kInvalidId || b_faction_id == kInvalidId) return false;
  if (a_faction_id == b_faction_id) return true;
  // Mutual friendly implies full cooperation, which includes trade/logistics access.
  if (are_factions_mutual_friendly(a_faction_id, b_faction_id)) return true;

  // Otherwise, allow trade access if a Trade Agreement treaty exists (or stronger).
  TreatyType tt = TreatyType::Ceasefire;
  if (!sim_internal::strongest_active_treaty_between(state_, a_faction_id, b_faction_id, &tt)) return false;
  return tt == TreatyType::TradeAgreement || tt == TreatyType::Alliance;
}


Id Simulation::create_treaty(Id faction_a, Id faction_b, TreatyType type, int duration_days, bool push_event,
                             std::string* error) {
  if (error) error->clear();
  if (faction_a == kInvalidId || faction_b == kInvalidId) {
    if (error) *error = "invalid faction id";
    return kInvalidId;
  }
  if (faction_a == faction_b) {
    if (error) *error = "treaty requires two different factions";
    return kInvalidId;
  }

  const auto* fa = find_ptr(state_.factions, faction_a);
  const auto* fb = find_ptr(state_.factions, faction_b);
  if (!fa || !fb) {
    if (error) *error = "unknown faction";
    return kInvalidId;
  }

  Id a = faction_a;
  Id b = faction_b;
  normalize_faction_pair(a, b);

  // Normalize duration: 0 => indefinite, <0 => indefinite, >0 => clamped to at least 1 day.
  if (duration_days == 0) duration_days = -1;
  if (duration_days > 0) duration_days = std::max(1, duration_days);

  const std::int64_t now = state_.date.days_since_epoch();

  auto maybe_sync_intel = [&](TreatyType tt) {
    // Trade agreements and alliances should actually do something tangible:
    // exchange star charts (discovered systems + surveyed jump points), and for
    // alliances, also share contact intel.
    const bool share_map = (tt == TreatyType::Alliance || tt == TreatyType::TradeAgreement || tt == TreatyType::ResearchAgreement);
    const bool share_contacts = (tt == TreatyType::Alliance);
    if (!share_map) return;

    const auto d = sync_intel_between_factions(state_, a, b, share_contacts);
    if (d.route_cache_dirty) invalidate_jump_route_cache();

    if (!push_event) return;

    const auto& name_a = state_.factions.at(a).name;
    const auto& name_b = state_.factions.at(b).name;

    auto push_one = [&](Id fid, Id other, const std::string& other_name, int add_sys, int add_jumps, int add_contacts) {
      if (add_sys + add_jumps + add_contacts <= 0) return;
      std::string msg = "Intel shared (";
      msg += treaty_type_title(tt);
      msg += ") with ";
      msg += other_name;
      msg += ": +";
      msg += std::to_string(add_sys);
      msg += " systems, +";
      msg += std::to_string(add_jumps);
      msg += " jump surveys";
      if (share_contacts) {
        msg += ", +";
        msg += std::to_string(add_contacts);
        msg += " contacts";
      }
      this->push_event(EventLevel::Info, EventCategory::Intel, msg,
                       EventContext{.faction_id = fid,
                                    .faction_id2 = other,
                                    .system_id = kInvalidId,
                                    .ship_id = kInvalidId,
                                    .colony_id = kInvalidId});
    };

    push_one(a, b, name_b, d.added_a_systems, d.added_a_jumps, d.merged_a_contacts);
    push_one(b, a, name_a, d.added_b_systems, d.added_b_jumps, d.merged_b_contacts);
  };

  // Renew an existing treaty of the same type between the same factions.
  for (auto& [tid, t] : state_.treaties) {
    if (t.faction_a != a || t.faction_b != b) continue;
    if (t.type != type) continue;
    t.start_day = now;
    t.duration_days = duration_days;

    if (push_event) {
      std::string msg = "Treaty renewed: ";
      msg += treaty_type_title(type);
      msg += " between ";
      msg += state_.factions.at(a).name;
      msg += " and ";
      msg += state_.factions.at(b).name;
      if (duration_days > 0) {
        msg += " (";
        msg += std::to_string(duration_days);
        msg += " days)";
      } else {
        msg += " (indefinite)";
      }
      this->push_event(EventLevel::Info, EventCategory::Diplomacy, msg,
                 EventContext{.faction_id = a, .faction_id2 = b, .system_id = kInvalidId, .ship_id = kInvalidId, .colony_id = kInvalidId});
    }

    // Treaties can be signed long after each side has explored. Sync intel on
    // renew so that long-running pacts don't require manual stance toggles to
    // exchange star charts.
    maybe_sync_intel(type);

    return tid;
  }

  Treaty t;
  t.id = allocate_id(state_);
  t.faction_a = a;
  t.faction_b = b;
  t.type = type;
  t.start_day = now;
  t.duration_days = duration_days;

  state_.treaties[t.id] = t;

  if (push_event) {
    std::string msg = "Treaty signed: ";
    msg += treaty_type_title(type);
    msg += " between ";
    msg += state_.factions.at(a).name;
    msg += " and ";
    msg += state_.factions.at(b).name;
    if (duration_days > 0) {
      msg += " (";
      msg += std::to_string(duration_days);
      msg += " days)";
    } else {
      msg += " (indefinite)";
    }
    this->push_event(EventLevel::Info, EventCategory::Diplomacy, msg,
               EventContext{.faction_id = a, .faction_id2 = b, .system_id = kInvalidId, .ship_id = kInvalidId, .colony_id = kInvalidId});
  }

  // Immediately exchange intel implied by the treaty.
  maybe_sync_intel(type);

  return t.id;
}

bool Simulation::cancel_treaty(Id treaty_id, bool push_event, std::string* error) {
  if (error) error->clear();
  if (treaty_id == kInvalidId) {
    if (error) *error = "invalid treaty id";
    return false;
  }
  auto it = state_.treaties.find(treaty_id);
  if (it == state_.treaties.end()) {
    if (error) *error = "treaty not found";
    return false;
  }

  const Treaty t = it->second;
  state_.treaties.erase(it);

  if (push_event) {
    std::string msg = "Treaty cancelled: ";
    msg += treaty_type_title(t.type);
    msg += " between ";
    msg += state_.factions.at(t.faction_a).name;
    msg += " and ";
    msg += state_.factions.at(t.faction_b).name;
    this->push_event(EventLevel::Info, EventCategory::Diplomacy, msg,
               EventContext{.faction_id = t.faction_a, .faction_id2 = t.faction_b, .system_id = kInvalidId, .ship_id = kInvalidId, .colony_id = kInvalidId});
  }

  return true;
}

std::vector<Treaty> Simulation::treaties_between(Id faction_a, Id faction_b) const {
  std::vector<Treaty> out;
  if (faction_a == kInvalidId || faction_b == kInvalidId) return out;
  if (faction_a == faction_b) return out;
  if (state_.treaties.empty()) return out;

  Id a = faction_a;
  Id b = faction_b;
  normalize_faction_pair(a, b);

  for (const auto& [tid, t] : state_.treaties) {
    (void)tid;
    if (t.faction_a == a && t.faction_b == b) out.push_back(t);
  }

  std::sort(out.begin(), out.end(), [](const Treaty& x, const Treaty& y) { return x.id < y.id; });
  return out;
}


std::vector<DiplomaticOffer> Simulation::diplomatic_offers_between(Id faction_a, Id faction_b) const {
  std::vector<DiplomaticOffer> out;
  if (faction_a == kInvalidId || faction_b == kInvalidId) return out;
  if (faction_a == faction_b) return out;
  if (state_.diplomatic_offers.empty()) return out;

  for (const auto& [oid, o] : state_.diplomatic_offers) {
    (void)oid;
    if ((o.from_faction_id == faction_a && o.to_faction_id == faction_b) ||
        (o.from_faction_id == faction_b && o.to_faction_id == faction_a)) {
      out.push_back(o);
    }
  }

  std::sort(out.begin(), out.end(), [](const DiplomaticOffer& x, const DiplomaticOffer& y) { return x.id < y.id; });
  return out;
}

std::vector<DiplomaticOffer> Simulation::incoming_diplomatic_offers(Id to_faction_id) const {
  std::vector<DiplomaticOffer> out;
  if (to_faction_id == kInvalidId) return out;
  if (state_.diplomatic_offers.empty()) return out;

  for (const auto& [oid, o] : state_.diplomatic_offers) {
    (void)oid;
    if (o.to_faction_id == to_faction_id) out.push_back(o);
  }

  std::sort(out.begin(), out.end(), [](const DiplomaticOffer& x, const DiplomaticOffer& y) { return x.id < y.id; });
  return out;
}

Id Simulation::create_diplomatic_offer(Id from_faction_id, Id to_faction_id, TreatyType treaty_type,
                                       int treaty_duration_days, int offer_expires_in_days,
                                       bool push_event, std::string* error, const std::string& message) {
  if (error) error->clear();

  if (from_faction_id == kInvalidId || to_faction_id == kInvalidId) {
    if (error) *error = "Invalid faction id.";
    return kInvalidId;
  }
  if (from_faction_id == to_faction_id) {
    if (error) *error = "Cannot create an offer to self.";
    return kInvalidId;
  }
  const auto* from = find_ptr(state_.factions, from_faction_id);
  const auto* to = find_ptr(state_.factions, to_faction_id);
  if (!from || !to) {
    if (error) *error = "One or both factions do not exist.";
    return kInvalidId;
  }

  // Do not allow duplicate pending offers of the same type.
  for (const auto& [oid, o] : state_.diplomatic_offers) {
    (void)oid;
    if (o.from_faction_id == from_faction_id && o.to_faction_id == to_faction_id && o.treaty_type == treaty_type) {
      if (error) *error = "An offer of this type is already pending.";
      return kInvalidId;
    }
  }

  // Do not offer a treaty that is already active.
  Id a = from_faction_id;
  Id b = to_faction_id;
  normalize_faction_pair(a, b);
  for (const auto& [tid, t] : state_.treaties) {
    (void)tid;
    if (t.faction_a == a && t.faction_b == b && t.type == treaty_type) {
      if (error) *error = "A treaty of this type is already active.";
      return kInvalidId;
    }
  }

  if (treaty_duration_days == 0) treaty_duration_days = 1;

  const int now_day = static_cast<int>(state_.date.days_since_epoch());
  const int expire_day = (offer_expires_in_days <= 0) ? -1 : (now_day + std::max(1, offer_expires_in_days));

  DiplomaticOffer offer;
  offer.id = allocate_id(state_);
  offer.from_faction_id = from_faction_id;
  offer.to_faction_id = to_faction_id;
  offer.treaty_type = treaty_type;
  offer.treaty_duration_days = treaty_duration_days;
  offer.created_day = now_day;
  offer.expire_day = expire_day;
  offer.message = message;

  state_.diplomatic_offers[offer.id] = offer;

  if (push_event) {
    EventContext ctx;
    ctx.faction_id = to_faction_id;
    ctx.faction_id2 = from_faction_id;

    std::string msg = "Diplomatic offer received from " + from->name + ": ";
    msg += treaty_type_title(treaty_type);

    if (treaty_duration_days < 0) {
      msg += " (indefinite)";
    } else {
      msg += " (" + std::to_string(treaty_duration_days) + " days)";
    }

    if (expire_day >= 0) {
      msg += " [expires in " + std::to_string(std::max(0, expire_day - now_day)) + " days]";
    }

    if (!offer.message.empty()) {
      // Keep the log readable; show a short preview of the attached note.
      std::string preview = offer.message;
      constexpr std::size_t kMaxPreview = 120;
      if (preview.size() > kMaxPreview) {
        preview.resize(kMaxPreview);
        preview += "...";
      }
      msg += " — \"" + preview + "\"";
    }

    this->push_event(EventLevel::Info, EventCategory::Diplomacy, std::move(msg), ctx);
  }

  return offer.id;
}

bool Simulation::accept_diplomatic_offer(Id offer_id, bool push_event, std::string* error) {
  if (error) error->clear();

  auto it = state_.diplomatic_offers.find(offer_id);
  if (it == state_.diplomatic_offers.end()) {
    if (error) *error = "Offer not found.";
    return false;
  }

  const DiplomaticOffer offer = it->second;

  // Ensure factions still exist.
  const auto* from = find_ptr(state_.factions, offer.from_faction_id);
  const auto* to = find_ptr(state_.factions, offer.to_faction_id);
  if (!from || !to) {
    if (error) *error = "One or both factions no longer exist.";
    return false;
  }

  const Id treaty_id = create_treaty(offer.from_faction_id, offer.to_faction_id, offer.treaty_type,
                                     offer.treaty_duration_days, push_event, error);
  if (treaty_id == kInvalidId) return false;

  // Remove the offer after the treaty is created.
  state_.diplomatic_offers.erase(it);

  // Anti-spam cooldown for both factions.
  const int now_day = static_cast<int>(state_.date.days_since_epoch());
  constexpr int kCooldownDays = 60;

  if (auto* f = find_ptr(state_.factions, offer.from_faction_id)) {
    int& until = f->diplomacy_offer_cooldown_until_day[offer.to_faction_id];
    until = std::max(until, now_day + kCooldownDays);
  }
  if (auto* f = find_ptr(state_.factions, offer.to_faction_id)) {
    int& until = f->diplomacy_offer_cooldown_until_day[offer.from_faction_id];
    until = std::max(until, now_day + kCooldownDays);
  }

  return true;
}

bool Simulation::decline_diplomatic_offer(Id offer_id, bool push_event, std::string* error) {
  if (error) error->clear();

  auto it = state_.diplomatic_offers.find(offer_id);
  if (it == state_.diplomatic_offers.end()) {
    if (error) *error = "Offer not found.";
    return false;
  }

  const DiplomaticOffer offer = it->second;
  const auto* from = find_ptr(state_.factions, offer.from_faction_id);
  const auto* to = find_ptr(state_.factions, offer.to_faction_id);

  state_.diplomatic_offers.erase(it);

  const int now_day = static_cast<int>(state_.date.days_since_epoch());
  constexpr int kCooldownDays = 90;

  if (auto* f = find_ptr(state_.factions, offer.from_faction_id)) {
    int& until = f->diplomacy_offer_cooldown_until_day[offer.to_faction_id];
    until = std::max(until, now_day + kCooldownDays);
  }

  if (push_event) {
    EventContext ctx;
    ctx.faction_id = offer.to_faction_id;
    ctx.faction_id2 = offer.from_faction_id;

    std::string msg = "Diplomatic offer declined: ";
    if (to) msg += to->name + " declined ";
    msg += treaty_type_title(offer.treaty_type);
    if (from) msg += " proposed by " + from->name;
    this->push_event(EventLevel::Info, EventCategory::Diplomacy, std::move(msg), ctx);
  }

  return true;
}


FactionScoreBreakdown Simulation::compute_faction_score(Id faction_id) const {
  return compute_faction_score(faction_id, state_.victory_rules);
}

FactionScoreBreakdown Simulation::compute_faction_score(Id faction_id, const VictoryRules& rules) const {
  FactionScoreBreakdown out;
  const auto* fac = find_ptr(state_.factions, faction_id);
  if (!fac) return out;

  int colony_count = 0;
  double pop_million = 0.0;
  double installation_cost = 0.0;

  for (const auto& [cid, c] : state_.colonies) {
    (void)cid;
    if (c.faction_id != faction_id) continue;
    colony_count++;
    pop_million += c.population_millions;

    for (const auto& [inst_id, count] : c.installations) {
      if (count <= 0) continue;
      auto it = content_.installations.find(inst_id);
      if (it == content_.installations.end()) continue;
      installation_cost += it->second.construction_cost * static_cast<double>(count);
    }
  }

  double ship_mass_tons = 0.0;
  for (const auto& [sid, sh] : state_.ships) {
    (void)sid;
    if (sh.faction_id != faction_id) continue;
    if (const ShipDesign* d = find_design(sh.design_id)) ship_mass_tons += d->mass_tons;
  }

  const double tech_count = static_cast<double>(fac->known_techs.size());
  const double discovered_systems = static_cast<double>(fac->discovered_systems.size());
  const double discovered_anomalies = static_cast<double>(fac->discovered_anomalies.size());

  out.colonies_points = static_cast<double>(colony_count) * rules.score_colony_points;
  out.population_points = pop_million * rules.score_population_per_million;
  out.installations_points = installation_cost * rules.score_installation_cost_mult;
  out.ships_points = ship_mass_tons * rules.score_ship_mass_ton_mult;
  out.tech_points = tech_count * rules.score_known_tech_points;
  out.exploration_points = discovered_systems * rules.score_discovered_system_points +
                           discovered_anomalies * rules.score_discovered_anomaly_points;

  return out;
}

std::vector<ScoreboardEntry> Simulation::compute_scoreboard() const {
  return compute_scoreboard(state_.victory_rules);
}

std::vector<ScoreboardEntry> Simulation::compute_scoreboard(const VictoryRules& rules) const {
  std::vector<ScoreboardEntry> out;
  out.reserve(state_.factions.size());

  // Pre-aggregate a few values in one pass for performance.
  std::unordered_map<Id, int> colony_counts;
  std::unordered_map<Id, double> pops;
  std::unordered_map<Id, double> inst_costs;
  colony_counts.reserve(state_.factions.size() * 2 + 8);
  pops.reserve(state_.factions.size() * 2 + 8);
  inst_costs.reserve(state_.factions.size() * 2 + 8);

  for (const auto& [cid, c] : state_.colonies) {
    (void)cid;
    if (c.faction_id == kInvalidId) continue;
    colony_counts[c.faction_id] += 1;
    pops[c.faction_id] += c.population_millions;
    if (rules.score_installation_cost_mult > 0.0) {
      double cost = 0.0;
      for (const auto& [inst_id, count] : c.installations) {
        if (count <= 0) continue;
        auto it = content_.installations.find(inst_id);
        if (it == content_.installations.end()) continue;
        cost += it->second.construction_cost * static_cast<double>(count);
      }
      if (cost != 0.0) inst_costs[c.faction_id] += cost;
    }
  }

  std::unordered_map<Id, double> ship_mass;
  ship_mass.reserve(state_.factions.size() * 2 + 8);
  for (const auto& [sid, sh] : state_.ships) {
    (void)sid;
    if (sh.faction_id == kInvalidId) continue;
    if (const ShipDesign* d = find_design(sh.design_id)) ship_mass[sh.faction_id] += d->mass_tons;
  }

  for (Id fid : sorted_keys(state_.factions)) {
    const auto& f = state_.factions.at(fid);
    ScoreboardEntry e;
    e.faction_id = fid;
    e.faction_name = f.name;
    e.control = f.control;
    // Passive factions (neutral ambient entities, e.g. Merchant Guild) are not
    // intended to participate in victory conditions.
    e.eligible_for_victory =
        !(rules.exclude_pirates && f.control == FactionControl::AI_Pirate) &&
        (f.control != FactionControl::AI_Passive);

    const int colonies = colony_counts.contains(fid) ? colony_counts.at(fid) : 0;
    const bool has_colony = colonies > 0;
    const bool has_ship = ship_mass.contains(fid) && ship_mass.at(fid) > 0.0;
    e.alive = rules.elimination_requires_colony ? has_colony : (has_colony || has_ship);

    // Build score breakdown using aggregated values.
    e.score.colonies_points = static_cast<double>(colonies) * rules.score_colony_points;
    e.score.population_points = (pops.contains(fid) ? pops.at(fid) : 0.0) * rules.score_population_per_million;
    e.score.installations_points = (inst_costs.contains(fid) ? inst_costs.at(fid) : 0.0) * rules.score_installation_cost_mult;
    e.score.ships_points = (ship_mass.contains(fid) ? ship_mass.at(fid) : 0.0) * rules.score_ship_mass_ton_mult;
    e.score.tech_points = static_cast<double>(f.known_techs.size()) * rules.score_known_tech_points;
    e.score.exploration_points = static_cast<double>(f.discovered_systems.size()) * rules.score_discovered_system_points +
                                 static_cast<double>(f.discovered_anomalies.size()) * rules.score_discovered_anomaly_points;

    out.push_back(std::move(e));
  }

  std::sort(out.begin(), out.end(), [](const ScoreboardEntry& a, const ScoreboardEntry& b) {
    const double at = a.score.total_points();
    const double bt = b.score.total_points();
    if (at != bt) return at > bt;
    return a.faction_id < b.faction_id;
  });

  return out;
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

  // Escalating to Hostile breaks any active treaties between the two factions.
  if (status == DiplomacyStatus::Hostile && !state_.treaties.empty()) {
    Id ta = from_faction_id;
    Id tb = to_faction_id;
    normalize_faction_pair(ta, tb);
    std::vector<Id> to_break;
    to_break.reserve(state_.treaties.size());
    for (const auto& [tid, t] : state_.treaties) {
      if (t.faction_a == ta && t.faction_b == tb) to_break.push_back(tid);
    }
    for (Id tid : to_break) {
      (void)cancel_treaty(tid, /*push_event=*/push_event_on_change, nullptr);
    }
  }


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
        } else {
          Contact& d = it_dst->second;
          if (c.last_seen_day > d.last_seen_day) {
            d = c;
            merged += 1;
          } else if (c.last_seen_day == d.last_seen_day) {
            // If both contacts are from the same day, prefer a more informative track.
            // Priority:
            // 1) Has a 2-point track (velocity estimate)
            // 2) Lower uncertainty at last detection (tighter last-known estimate)
            const bool c_has_track = (c.prev_seen_day >= 0 && c.prev_seen_day < c.last_seen_day);
            const bool d_has_track = (d.prev_seen_day >= 0 && d.prev_seen_day < d.last_seen_day);

            bool replace = false;
            if (c_has_track != d_has_track) {
              replace = c_has_track;
            } else {
              const double cu = c.last_seen_position_uncertainty_mkm;
              const double du = d.last_seen_position_uncertainty_mkm;
              if (std::isfinite(cu) && std::isfinite(du)) {
                // Prefer a strictly smaller positive uncertainty, or prefer a defined
                // uncertainty over an implicit "unknown" (0).
                if (cu > 0.0 && du > 0.0) {
                  replace = (cu < du);
                } else if (cu > 0.0 && du <= 0.0) {
                  replace = true;
                }
              } else if (std::isfinite(cu) && !std::isfinite(du)) {
                replace = true;
              }
            }

            if (replace) {
              d = c;
              merged += 1;
            }
          }
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
      this->push_event(EventLevel::Info, EventCategory::Diplomacy, msg, ctx);
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
  const double ecm = d ? std::max(0.0, d->ecm_strength) : 0.0;

  // Fast path: legacy behavior.
  bool use_los = cfg_.enable_sensor_los_attenuation;
  bool use_body = cfg_.enable_body_occlusion_sensors;

  const auto* sys = (use_los || use_body) ? find_ptr(state_.systems, tgt->system_id) : nullptr;
  if ((use_los || use_body) && !sys) {
    use_los = false;
    use_body = false;
  }

  if (use_los) {
    const bool micro = (cfg_.enable_nebula_microfields && sys->nebula_density > 1e-6 && cfg_.nebula_microfield_strength > 1e-9);
    const bool storm_cells = (cfg_.enable_nebula_storms && cfg_.enable_nebula_storm_cells && system_has_storm(tgt->system_id) &&
                              cfg_.nebula_storm_cell_strength > 1e-9 && cfg_.nebula_storm_sensor_penalty > 1e-9);
    if (!micro && !storm_cells) use_los = false;
  }

  if (use_body) {
    if (!sys || sys->bodies.empty()) use_body = false;
  }

  if (!use_los && !use_body) {
    return any_source_detects(sources, tgt->position_mkm, sig, ecm);
  }

  // LOS/body path: same base sensor math, but with optional relative LOS multiplier
  // and optional geometric occlusion by planetary bodies.
  double sig01 = std::isfinite(sig) ? sig : 1.0;
  if (sig01 < 0.0) sig01 = 0.0;
  double ecm01 = std::isfinite(ecm) ? ecm : 0.0;
  if (ecm01 < 0.0) ecm01 = 0.0;

  const double pad_mkm = std::max(0.0, cfg_.body_occlusion_padding_mkm);

  for (const auto& src : sources) {
    if (src.range_mkm <= 0.0) continue;

    double eccm = std::isfinite(src.eccm_strength) ? src.eccm_strength : 0.0;
    if (eccm < 0.0) eccm = 0.0;

    double ew_mult = (1.0 + eccm) / (1.0 + ecm01);
    if (!std::isfinite(ew_mult)) ew_mult = 1.0;
    ew_mult = std::clamp(ew_mult, 0.1, 10.0);

    const double r_base = src.range_mkm * sig01 * ew_mult;
    if (r_base <= 1e-9) continue;

    const Vec2 dp = tgt->position_mkm - src.pos_mkm;
    const double d2 = dp.x * dp.x + dp.y * dp.y;
    if (d2 > r_base * r_base + 1e-9) continue;

    if (use_body) {
      if (sim_internal::system_line_of_sight_blocked_by_bodies(state_, tgt->system_id, src.pos_mkm, tgt->position_mkm, pad_mkm)) {
        continue;
      }
    }

    double r = r_base;
    if (use_los) {
      const std::uint64_t extra_seed = procgen_obscure::splitmix64(static_cast<std::uint64_t>(src.ship_id) * 0x9E3779B97F4A7C15ULL ^
                                                            static_cast<std::uint64_t>(target_ship_id) * 0xBF58476D1CE4E5B9ULL);
      const double los = this->system_sensor_environment_multiplier_los(tgt->system_id, src.pos_mkm, tgt->position_mkm, extra_seed);
      r = r_base * los;
      if (r <= 1e-9) continue;
      if (d2 > r * r + 1e-9) continue;
    }

    return true;
  }

  return false;
}

std::vector<Id> Simulation::detected_hostile_ships_in_system(Id viewer_faction_id, Id system_id) const {
  std::vector<Id> out;
  const auto* sys = find_ptr(state_.systems, system_id);
  if (!sys) return out;

  const auto sources = gather_sensor_sources(*this, viewer_faction_id, system_id);
  if (sources.empty()) return out;

  bool use_los = cfg_.enable_sensor_los_attenuation;
  if (use_los) {
    const bool micro = (cfg_.enable_nebula_microfields && sys->nebula_density > 1e-6 && cfg_.nebula_microfield_strength > 1e-9);
    const bool storm_cells = (cfg_.enable_nebula_storms && cfg_.enable_nebula_storm_cells && system_has_storm(system_id) &&
                              cfg_.nebula_storm_cell_strength > 1e-9 && cfg_.nebula_storm_sensor_penalty > 1e-9);
    if (!micro && !storm_cells) use_los = false;
  }

  const bool use_body = cfg_.enable_body_occlusion_sensors && !sys->bodies.empty();
  const double pad_mkm = (use_body && std::isfinite(cfg_.body_occlusion_padding_mkm) && cfg_.body_occlusion_padding_mkm > 0.0)
                             ? cfg_.body_occlusion_padding_mkm
                             : 0.0;

  // Use a spatial index to avoid scanning every ship for every query.
  SpatialIndex2D idx;
  idx.build_from_ship_ids(sys->ships, state_.ships);

  const double max_sig = sim_sensors::max_signature_multiplier_for_detection(*this);

  auto sane_nonneg = [](double x, double fallback) {
    if (!std::isfinite(x)) return fallback;
    if (x < 0.0) return 0.0;
    return x;
  };

  for (const auto& src : sources) {
    if (src.range_mkm <= 1e-9) continue;

    // EW multiplier can increase effective range (ECCM) or reduce it (target ECM).
    // For the spatial index broadphase, assume target ECM = 0 (worst case range),
    // and clamp to match sim_sensors::any_source_detects().
    const double src_eccm = sane_nonneg(src.eccm_strength, 0.0);
    double max_ew_mult = (1.0 + src_eccm) / 1.0;
    if (!std::isfinite(max_ew_mult)) max_ew_mult = 1.0;
    max_ew_mult = std::clamp(max_ew_mult, 0.1, 10.0);

    const auto nearby = idx.query_radius(src.pos_mkm, src.range_mkm * max_sig * max_ew_mult, 1e-9);
    for (Id sid : nearby) {
      const auto* sh = find_ptr(state_.ships, sid);
      if (!sh) continue;
      if (sh->system_id != system_id) continue;
      if (sh->faction_id == viewer_faction_id) continue;
      if (!are_factions_hostile(viewer_faction_id, sh->faction_id)) continue;

      // Apply target signature/stealth + EMCON and electronic warfare.
      const auto* d = find_design(sh->design_id);
      const double sig = sim_sensors::effective_signature_multiplier(*this, *sh, d);

      // Apply electronic warfare (ECCM vs ECM) consistently with any_source_detects().
      const double tgt_ecm = d ? sane_nonneg(d->ecm_strength, 0.0) : 0.0;
      double ew_mult = (1.0 + src_eccm) / (1.0 + tgt_ecm);
      if (!std::isfinite(ew_mult)) ew_mult = 1.0;
      ew_mult = std::clamp(ew_mult, 0.1, 10.0);

      const double eff = src.range_mkm * sig * ew_mult;
      if (eff <= 1e-9) continue;
      const double dx = sh->position_mkm.x - src.pos_mkm.x;
      const double dy = sh->position_mkm.y - src.pos_mkm.y;
      const double d2 = dx * dx + dy * dy;
      if (d2 > eff * eff + 1e-9) continue;

      // Hard geometric occlusion by bodies (planets/moons/etc.).
      if (use_body) {
        if (sim_internal::system_line_of_sight_blocked_by_bodies(state_, system_id, src.pos_mkm, sh->position_mkm, pad_mkm)) {
          continue;
        }
      }

      if (use_los) {
        const std::uint64_t extra_seed = procgen_obscure::splitmix64(static_cast<std::uint64_t>(src.ship_id) * 0xD6E8FEB86659FD93ULL ^
                                                                static_cast<std::uint64_t>(sid) * 0x2545F4914F6CDD1DULL);
        const double los = this->system_sensor_environment_multiplier_los(system_id, src.pos_mkm, sh->position_mkm, extra_seed);
        const double eff_los = eff * los;
        if (eff_los <= 1e-9) continue;
        if (d2 > eff_los * eff_los + 1e-9) continue;
      }

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

double Simulation::contact_uncertainty_radius_mkm(const Contact& c, int now_day) const {
  if (!cfg_.enable_contact_uncertainty) return 0.0;
  if (now_day < 0) now_day = 0;

  const int age = std::max(0, now_day - c.last_seen_day);

  double base = c.last_seen_position_uncertainty_mkm;
  if (!std::isfinite(base) || base < 0.0) base = 0.0;

  // Estimate target speed from the contact track when available; otherwise,
  // fall back to the last-seen design's nominal speed.
  double sp_mkm_per_day = 0.0;
  if (c.prev_seen_day >= 0 && c.prev_seen_day < c.last_seen_day) {
    const int dt = c.last_seen_day - c.prev_seen_day;
    if (dt > 0) {
      const Vec2 v = (c.last_seen_position_mkm - c.prev_seen_position_mkm) * (1.0 / static_cast<double>(dt));
      if (std::isfinite(v.x) && std::isfinite(v.y)) {
        sp_mkm_per_day = v.length();
      }
    }
  }
  if (sp_mkm_per_day <= 1e-12) {
    if (!c.last_seen_design_id.empty()) {
      if (const auto* d = find_design(c.last_seen_design_id)) {
        sp_mkm_per_day = mkm_per_day_from_speed(d->speed_km_s, cfg_.seconds_per_day);
      }
    }
  }

  // If we still don't have a speed estimate (unknown design and no velocity track),
  // fall back to a conservative assumed speed so uncertainty continues to grow.
  if (sp_mkm_per_day <= 1e-12) {
    const double sp_km_s = std::max(0.0, cfg_.contact_uncertainty_unknown_speed_km_s);
    sp_mkm_per_day = mkm_per_day_from_speed(sp_km_s, cfg_.seconds_per_day);
  }

  if (!std::isfinite(sp_mkm_per_day) || sp_mkm_per_day < 0.0) sp_mkm_per_day = 0.0;

  double growth = std::max(0.0, cfg_.contact_uncertainty_growth_fraction_of_speed) * sp_mkm_per_day;
  if (!std::isfinite(growth) || growth < 0.0) growth = 0.0;
  growth = std::max(growth, std::max(0.0, cfg_.contact_uncertainty_growth_min_mkm_per_day));

  double rad = base + growth * static_cast<double>(age);
  if (!std::isfinite(rad) || rad < 0.0) rad = 0.0;

  const double cap = cfg_.contact_uncertainty_max_mkm;
  if (std::isfinite(cap) && cap > 0.0) {
    rad = std::min(rad, cap);
  }

  return rad;
}


} // namespace nebula4x
