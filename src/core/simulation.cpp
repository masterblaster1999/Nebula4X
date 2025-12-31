#include "nebula4x/core/simulation.h"

#include "simulation_internal.h"

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

struct PredictedNavState {
  Id system_id{kInvalidId};
  Vec2 position_mkm{0.0, 0.0};
};

// Predict which system/position a ship would be in after executing the queued
// TravelViaJump orders currently in its ShipOrders queue.
//
// This is a lightweight helper used for:
// - Shift-queue previews (UI)
// - Ensuring subsequent travel commands pathfind from the end-of-queue system.
PredictedNavState predicted_nav_state_after_queued_jumps(const GameState& s, Id ship_id,
                                                        bool include_queued_jumps) {
  const auto* ship = find_ptr(s.ships, ship_id);
  if (!ship) return PredictedNavState{};

  PredictedNavState out;
  out.system_id = ship->system_id;
  out.position_mkm = ship->position_mkm;
  if (!include_queued_jumps) return out;

  auto it = s.ship_orders.find(ship_id);
  if (it == s.ship_orders.end()) return out;

  Id sys = out.system_id;
  Vec2 pos = out.position_mkm;

  for (const auto& ord : it->second.queue) {
    if (!std::holds_alternative<TravelViaJump>(ord)) continue;
    const Id jump_id = std::get<TravelViaJump>(ord).jump_point_id;
    const auto* jp = find_ptr(s.jump_points, jump_id);
    if (!jp) continue;
    if (jp->system_id != sys) continue;
    if (jp->linked_jump_id == kInvalidId) continue;
    const auto* dest = find_ptr(s.jump_points, jp->linked_jump_id);
    if (!dest) continue;
    if (dest->system_id == kInvalidId) continue;
    if (!find_ptr(s.systems, dest->system_id)) continue;
    sys = dest->system_id;
    pos = dest->position_mkm;
  }

  out.system_id = sys;
  out.position_mkm = pos;
  return out;
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
  const double mkm_per_day = mkm_per_day_from_speed(speed_km_s, seconds_per_day);
  if (mkm_per_day <= 0.0) {
    plan.eta_days = std::numeric_limits<double>::infinity();
  } else {
    plan.eta_days = plan.distance_mkm / mkm_per_day;
  }
}



std::optional<JumpRoutePlan> compute_jump_route_plan(const Simulation& sim, Id start_system_id,
                                                    Vec2 start_pos_mkm, Id faction_id,
                                                    double speed_km_s, Id target_system_id,
                                                    bool restrict_to_discovered) {
  const auto& s = sim.state();

  if (!find_ptr(s.systems, start_system_id)) return std::nullopt;
  if (!find_ptr(s.systems, target_system_id)) return std::nullopt;
  // If we're restricted to a faction's discovered map (fog-of-war), prebuild a fast membership set.
  const Faction* fac = nullptr;
  std::unordered_set<Id> discovered;
  if (restrict_to_discovered) {
    fac = find_ptr(s.factions, faction_id);
    if (fac) {
      discovered.reserve(fac->discovered_systems.size() * 2 + 8);
      for (Id sid : fac->discovered_systems) discovered.insert(sid);
      // Ensure the starting system is always considered allowed.
      discovered.insert(start_system_id);
    }
  }

  auto allow_system = [&](Id sys_id) {
    if (!restrict_to_discovered) return true;
    // Backward-compat: if the faction doesn't exist, don't block routing.
    if (!fac) return true;
    return discovered.contains(sys_id);
  };

  if (restrict_to_discovered && !allow_system(target_system_id)) return std::nullopt;


  if (start_system_id == target_system_id) {
    JumpRoutePlan plan;
    plan.systems = {start_system_id};
    plan.distance_mkm = 0.0;
    plan.eta_days = 0.0;
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

  JumpRouteNode goal{kInvalidId, kInvalidId};

  while (!pq.empty()) {
    const JumpRoutePQItem cur = pq.top();
    pq.pop();

    const auto it_best = dist.find(cur.node);
    if (it_best == dist.end()) continue;
    if (dist_better(it_best->second, cur.d)) continue; // stale

    if (cur.node.system_id == target_system_id) {
      goal = cur.node;
      break;
    }

    const auto* sys = find_ptr(s.systems, cur.node.system_id);
    if (!sys) continue;

    Vec2 cur_pos = start_pos_mkm;
    if (cur.node.entry_jump_id != kInvalidId) {
      const auto* entry = find_ptr(s.jump_points, cur.node.entry_jump_id);
      if (!entry || entry->system_id != cur.node.system_id) continue;
      cur_pos = entry->position_mkm;
    }
    const std::vector<Id>& outgoing = outgoing_jumps(cur.node.system_id);

    for (Id jid : outgoing) {
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

  if (goal.system_id == kInvalidId) return std::nullopt;

  // Reconstruct jump ids.
  std::vector<Id> jump_ids;
  JumpRouteNode cur = goal;
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
  const auto it_goal = dist.find(goal);
  if (it_goal != dist.end()) plan.distance_mkm = it_goal->second.cost_mkm;

  const double mkm_per_day = mkm_per_day_from_speed(speed_km_s, sim.cfg().seconds_per_day);
  if (mkm_per_day > 0.0) {
    plan.eta_days = plan.distance_mkm / mkm_per_day;
  } else {
    plan.eta_days = std::numeric_limits<double>::infinity();
  }

  return plan;
}

struct SensorSource {
  Vec2 pos_mkm{0.0, 0.0};
  double range_mkm{0.0};
};


std::vector<SensorSource> gather_sensor_sources(const Simulation& sim, Id faction_id, Id system_id) {
  std::vector<SensorSource> sources;
  const auto& s = sim.state();
  const auto* sys = find_ptr(s.systems, system_id);
  if (!sys) return sources;

  // Friendly ship sensors in this system.
  for (Id sid : sys->ships) {
    const auto* sh = find_ptr(s.ships, sid);
    if (!sh) continue;
    if (sh->faction_id != faction_id) continue;
    const auto* d = sim.find_design(sh->design_id);
    double range = d ? d->sensor_range_mkm : 0.0;
    if (d && range > 0.0) {
      const auto p = compute_power_allocation(*d, sh->power_policy);
      if (!p.sensors_online) range = 0.0;
    }
    if (range <= 0.0) continue;
    sources.push_back(SensorSource{sh->position_mkm, range});
  }

  // Friendly colony-based sensors in this system.
  for (const auto& [_, c] : s.colonies) {
    if (c.faction_id != faction_id) continue;
    const auto* body = find_ptr(s.bodies, c.body_id);
    if (!body || body->system_id != system_id) continue;

    double best = 0.0;
    for (const auto& [inst_id, count] : c.installations) {
      if (count <= 0) continue;
      const auto it = sim.content().installations.find(inst_id);
      if (it == sim.content().installations.end()) continue;
      best = std::max(best, it->second.sensor_range_mkm);
    }

    if (best > 0.0) sources.push_back(SensorSource{body->position_mkm, best});
  }

  return sources;
}

bool any_source_detects(const std::vector<SensorSource>& sources, const Vec2& target_pos) {
  for (const auto& src : sources) {
    if (src.range_mkm <= 0.0) continue;
    const double dist = (target_pos - src.pos_mkm).length();
    if (dist <= src.range_mkm + 1e-9) return true;
  }
  return false;
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
                                                               bool restrict_to_discovered) const {
  ensure_jump_route_cache_current();

  JumpRouteCacheKey key;
  key.start_system_id = start_system_id;
  key.start_pos_x_bits = canonical_double_bits(start_pos_mkm.x);
  key.start_pos_y_bits = canonical_double_bits(start_pos_mkm.y);
  key.faction_id = faction_id;
  key.target_system_id = target_system_id;
  key.restrict_to_discovered = restrict_to_discovered;

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
                                      target_system_id, restrict_to_discovered);
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

bool Simulation::set_diplomatic_status(Id from_faction_id, Id to_faction_id, DiplomacyStatus status, bool reciprocal,
                                       bool push_event_on_change) {
  if (from_faction_id == kInvalidId || to_faction_id == kInvalidId) return false;
  if (from_faction_id == to_faction_id) return false;

  auto* a = find_ptr(state_.factions, from_faction_id);
  auto* b = find_ptr(state_.factions, to_faction_id);
  if (!a || !b) return false;

  const auto prev_a = diplomatic_status(from_faction_id, to_faction_id);
  const auto prev_b = diplomatic_status(to_faction_id, from_faction_id);

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
  return any_source_detects(sources, tgt->position_mkm);
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

  for (const auto& src : sources) {
    if (src.range_mkm <= 1e-9) continue;
    const auto nearby = idx.query_radius(src.pos_mkm, src.range_mkm, 1e-9);
    for (Id sid : nearby) {
      const auto* sh = find_ptr(state_.ships, sid);
      if (!sh) continue;
      if (sh->system_id != system_id) continue;
      if (sh->faction_id == viewer_faction_id) continue;
      if (!are_factions_hostile(viewer_faction_id, sh->faction_id)) continue;
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

void Simulation::apply_design_stats_to_ship(Ship& ship) {
  const ShipDesign* d = find_design(ship.design_id);
  if (!d) {
    ship.speed_km_s = 0.0;
    if (ship.hp <= 0.0) ship.hp = 1.0;
    ship.fuel_tons = 0.0;
    ship.shields = 0.0;
    return;
  }

  ship.speed_km_s = d->speed_km_s;
  if (ship.hp <= 0.0) ship.hp = d->max_hp;
  ship.hp = std::clamp(ship.hp, 0.0, d->max_hp);

  const double fuel_cap = std::max(0.0, d->fuel_capacity_tons);
  if (fuel_cap <= 1e-9) {
    ship.fuel_tons = 0.0;
  } else {
    // Initialize fuel for older saves / newly created ships.
    if (ship.fuel_tons < 0.0) ship.fuel_tons = fuel_cap;
    ship.fuel_tons = std::clamp(ship.fuel_tons, 0.0, fuel_cap);
  }

  const double max_sh = std::max(0.0, d->max_shields);
  if (max_sh <= 1e-9) {
    ship.shields = 0.0;
  } else {
    // Initialize shields for older saves / newly created ships.
    if (ship.shields < 0.0) ship.shields = max_sh;
    ship.shields = std::clamp(ship.shields, 0.0, max_sh);
  }

  const double troop_cap = std::max(0.0, d->troop_capacity);
  if (troop_cap <= 1e-9) {
    ship.troops = 0.0;
  } else {
    ship.troops = std::clamp(ship.troops, 0.0, troop_cap);
  }
}

bool Simulation::upsert_custom_design(ShipDesign design, std::string* error) {
  auto fail = [&](const std::string& msg) {
    if (error) *error = msg;
    return false;
  };

  if (design.id.empty()) return fail("Design id is empty");
  if (content_.designs.find(design.id) != content_.designs.end()) {
    return fail("Design id conflicts with built-in design: " + design.id);
  }
  if (design.name.empty()) design.name = design.id;

  double mass = 0.0;
  double speed = 0.0;
  double fuel_cap = 0.0;
  double fuel_use = 0.0;
  double cargo = 0.0;
  double sensor = 0.0;
  double colony_cap = 0.0;
  double troop_cap = 0.0;
  double weapon_damage = 0.0;
  double weapon_range = 0.0;
  double hp_bonus = 0.0;
  double max_shields = 0.0;
  double shield_regen = 0.0;

  // Power budgeting.
  double power_gen = 0.0;
  double power_use_total = 0.0;
  double power_use_engines = 0.0;
  double power_use_sensors = 0.0;
  double power_use_weapons = 0.0;
  double power_use_shields = 0.0;

  for (const auto& cid : design.components) {
    auto it = content_.components.find(cid);
    if (it == content_.components.end()) return fail("Unknown component id: " + cid);
    const auto& c = it->second;

    mass += c.mass_tons;
    speed = std::max(speed, c.speed_km_s);
    fuel_cap += c.fuel_capacity_tons;
    fuel_use += c.fuel_use_per_mkm;
    cargo += c.cargo_tons;
    sensor = std::max(sensor, c.sensor_range_mkm);
    colony_cap += c.colony_capacity_millions;
    troop_cap += c.troop_capacity;

    if (c.type == ComponentType::Weapon) {
      weapon_damage += c.weapon_damage;
      weapon_range = std::max(weapon_range, c.weapon_range_mkm);
    }

    if (c.type == ComponentType::Reactor) {
      power_gen += c.power_output;
    }
    power_use_total += c.power_use;
    if (c.type == ComponentType::Engine) power_use_engines += c.power_use;
    if (c.type == ComponentType::Sensor) power_use_sensors += c.power_use;
    if (c.type == ComponentType::Weapon) power_use_weapons += c.power_use;
    if (c.type == ComponentType::Shield) power_use_shields += c.power_use;

    hp_bonus += c.hp_bonus;

    if (c.type == ComponentType::Shield) {
      max_shields += c.shield_hp;
      shield_regen += c.shield_regen_per_day;
    }
  }

  design.mass_tons = mass;
  design.speed_km_s = speed;
  design.fuel_capacity_tons = fuel_cap;
  design.fuel_use_per_mkm = fuel_use;
  design.cargo_tons = cargo;
  design.sensor_range_mkm = sensor;
  design.colony_capacity_millions = colony_cap;
  design.troop_capacity = troop_cap;

  design.power_generation = power_gen;
  design.power_use_total = power_use_total;
  design.power_use_engines = power_use_engines;
  design.power_use_sensors = power_use_sensors;
  design.power_use_weapons = power_use_weapons;
  design.power_use_shields = power_use_shields;
  design.weapon_damage = weapon_damage;
  design.weapon_range_mkm = weapon_range;
  design.max_shields = max_shields;
  design.shield_regen_per_day = shield_regen;
  design.max_hp = std::max(1.0, mass * 2.0 + hp_bonus);

  state_.custom_designs[design.id] = std::move(design);
  return true;
}

void Simulation::initialize_unlocks_for_faction(Faction& f) {
  for (Id cid : sorted_keys(state_.colonies)) {
    const auto& col = state_.colonies.at(cid);
    if (col.faction_id != f.id) continue;

    if (const auto* body = find_ptr(state_.bodies, col.body_id)) {
      push_unique(f.discovered_systems, body->system_id);
    }

    for (const auto& [inst_id, count] : col.installations) {
      if (count <= 0) continue;
      push_unique(f.unlocked_installations, inst_id);
    }
  }

  for (Id sid : sorted_keys(state_.ships)) {
    const auto& ship = state_.ships.at(sid);
    if (ship.faction_id != f.id) continue;

    push_unique(f.discovered_systems, ship.system_id);

    if (const auto* d = find_design(ship.design_id)) {
      for (const auto& cid : d->components) push_unique(f.unlocked_components, cid);
    }
  }

  for (const auto& tech_id : f.known_techs) {
    auto tit = content_.techs.find(tech_id);
    if (tit == content_.techs.end()) continue;
    for (const auto& eff : tit->second.effects) {
      if (eff.type == "unlock_component") push_unique(f.unlocked_components, eff.value);
      if (eff.type == "unlock_installation") push_unique(f.unlocked_installations, eff.value);
    }
  }
}

void Simulation::remove_ship_from_fleets(Id ship_id) {
  if (ship_id == kInvalidId) return;
  if (state_.fleets.empty()) return;

  bool changed = false;
  for (auto& [_, fl] : state_.fleets) {
    const auto it = std::remove(fl.ship_ids.begin(), fl.ship_ids.end(), ship_id);
    if (it != fl.ship_ids.end()) {
      fl.ship_ids.erase(it, fl.ship_ids.end());
      changed = true;
    }
    if (fl.leader_ship_id == ship_id) {
      fl.leader_ship_id = kInvalidId;
      changed = true;
    }
  }

  if (changed) prune_fleets();
}

void Simulation::prune_fleets() {
  if (state_.fleets.empty()) return;

  // Deterministic pruning.
  const auto fleet_ids = sorted_keys(state_.fleets);

  // Enforce the invariant that a ship may belong to at most one fleet.
  std::unordered_set<Id> claimed;
  claimed.reserve(state_.ships.size() * 2);

  for (Id fleet_id : fleet_ids) {
    auto* fl = find_ptr(state_.fleets, fleet_id);
    if (!fl) continue;

    std::vector<Id> members;
    members.reserve(fl->ship_ids.size());
    for (Id sid : fl->ship_ids) {
      if (sid == kInvalidId) continue;
      const auto* sh = find_ptr(state_.ships, sid);
      if (!sh) continue;
      if (fl->faction_id != kInvalidId && sh->faction_id != fl->faction_id) continue;
      members.push_back(sid);
    }

    std::sort(members.begin(), members.end());
    members.erase(std::unique(members.begin(), members.end()), members.end());

    std::vector<Id> unique_members;
    unique_members.reserve(members.size());
    for (Id sid : members) {
      if (claimed.insert(sid).second) unique_members.push_back(sid);
    }

    fl->ship_ids = std::move(unique_members);

    if (!fl->ship_ids.empty()) {
      if (fl->leader_ship_id == kInvalidId ||
          std::find(fl->ship_ids.begin(), fl->ship_ids.end(), fl->leader_ship_id) == fl->ship_ids.end()) {
        fl->leader_ship_id = fl->ship_ids.front();
      }
    } else {
      fl->leader_ship_id = kInvalidId;
    }
  }

  for (auto it = state_.fleets.begin(); it != state_.fleets.end();) {
    if (it->second.ship_ids.empty()) {
      it = state_.fleets.erase(it);
    } else {
      ++it;
    }
  }
}

void Simulation::discover_system_for_faction(Id faction_id, Id system_id) {
  if (system_id == kInvalidId) return;
  auto* fac = find_ptr(state_.factions, faction_id);
  if (!fac) return;

  if (std::find(fac->discovered_systems.begin(), fac->discovered_systems.end(), system_id) !=
      fac->discovered_systems.end()) {
    return;
  }

  fac->discovered_systems.push_back(system_id);
  invalidate_jump_route_cache();

  const auto* sys = find_ptr(state_.systems, system_id);
  const std::string sys_name = sys ? sys->name : std::string("(unknown)");

  EventContext ctx;
  ctx.faction_id = faction_id;
  ctx.system_id = system_id;

  const std::string msg = fac->name + " discovered system " + sys_name;
  push_event(EventLevel::Info, EventCategory::Exploration, msg, ctx);
}

void Simulation::new_game() {
  state_ = make_sol_scenario();
  for (auto& [_, ship] : state_.ships) {
    apply_design_stats_to_ship(ship);
  }
  for (auto& [_, f] : state_.factions) initialize_unlocks_for_faction(f);
  recompute_body_positions();
  tick_contacts();
  invalidate_jump_route_cache();
}

void Simulation::load_game(GameState loaded) {
  state_ = std::move(loaded);

  {
    std::uint64_t max_seq = 0;
    for (const auto& ev : state_.events) max_seq = std::max(max_seq, ev.seq);
    if (state_.next_event_seq == 0) state_.next_event_seq = 1;
    if (state_.next_event_seq <= max_seq) state_.next_event_seq = max_seq + 1;
  }

  if (!state_.custom_designs.empty()) {
    std::vector<ShipDesign> designs;
    designs.reserve(state_.custom_designs.size());
    for (const auto& [_, d] : state_.custom_designs) designs.push_back(d);
    state_.custom_designs.clear();
    for (auto& d : designs) {
      std::string err;
      if (!upsert_custom_design(d, &err)) {
        nebula4x::log::warn(std::string("Custom design '") + d.id + "' could not be re-derived: " + err);
        state_.custom_designs[d.id] = d; 
      }
    }
  }

  for (auto& [_, ship] : state_.ships) {
    apply_design_stats_to_ship(ship);
  }

  for (auto& [_, f] : state_.factions) {
    initialize_unlocks_for_faction(f);
  }

  // Older saves (or hand-edited JSON) may contain stale fleet references.
  // Clean them up on load.
  prune_fleets();

  recompute_body_positions();
  tick_contacts();
  invalidate_jump_route_cache();
}

void Simulation::advance_days(int days) {
  if (days <= 0) return;
  for (int i = 0; i < days; ++i) tick_one_day();
}

namespace {

bool event_matches_stop(const SimEvent& ev, const EventStopCondition& stop) {
  const bool level_ok = (ev.level == EventLevel::Info && stop.stop_on_info) ||
                        (ev.level == EventLevel::Warn && stop.stop_on_warn) ||
                        (ev.level == EventLevel::Error && stop.stop_on_error);
  if (!level_ok) return false;

  if (stop.filter_category && ev.category != stop.category) return false;

  if (stop.faction_id != kInvalidId) {
    if (ev.faction_id != stop.faction_id && ev.faction_id2 != stop.faction_id) return false;
  }

  if (stop.system_id != kInvalidId) {
    if (ev.system_id != stop.system_id) return false;
  }

  if (stop.ship_id != kInvalidId) {
    if (ev.ship_id != stop.ship_id) return false;
  }

  if (stop.colony_id != kInvalidId) {
    if (ev.colony_id != stop.colony_id) return false;
  }

  if (!stop.message_contains.empty()) {
    const auto it = std::search(
        ev.message.begin(), ev.message.end(),
        stop.message_contains.begin(), stop.message_contains.end(),
        [](char a, char b) {
          return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
        });
    if (it == ev.message.end()) return false;
  }

  return true;
}

} // namespace

AdvanceUntilEventResult Simulation::advance_until_event(int max_days, const EventStopCondition& stop) {
  AdvanceUntilEventResult out;
  if (max_days <= 0) return out;

  std::uint64_t last_seq = 0;
  if (state_.next_event_seq > 0) last_seq = state_.next_event_seq - 1;

  for (int i = 0; i < max_days; ++i) {
    tick_one_day();
    out.days_advanced += 1;

    const std::uint64_t newest_seq = (state_.next_event_seq > 0) ? (state_.next_event_seq - 1) : 0;
    if (newest_seq <= last_seq) continue; 

    for (int j = static_cast<int>(state_.events.size()) - 1; j >= 0; --j) {
      const auto& ev = state_.events[static_cast<std::size_t>(j)];
      if (ev.seq <= last_seq) break;
      if (!event_matches_stop(ev, stop)) continue;
      out.hit = true;
      out.event = ev; // copy
      return out;
    }

    last_seq = newest_seq;
  }

  return out;
}

bool Simulation::clear_orders(Id ship_id) {
  if (!find_ptr(state_.ships, ship_id)) return false;
  auto& so = state_.ship_orders[ship_id];
  so.queue.clear();
  so.repeat = false;
  so.repeat_count_remaining = 0;
  so.repeat_template.clear();
  return true;
}

bool Simulation::enable_order_repeat(Id ship_id, int repeat_count_remaining) {
  if (!find_ptr(state_.ships, ship_id)) return false;
  auto& so = state_.ship_orders[ship_id];
  if (so.queue.empty()) return false;
  so.repeat = true;
  if (repeat_count_remaining < -1) repeat_count_remaining = -1;
  so.repeat_count_remaining = repeat_count_remaining;
  so.repeat_template = so.queue;
  return true;
}

bool Simulation::update_order_repeat_template(Id ship_id) {
  if (!find_ptr(state_.ships, ship_id)) return false;
  auto& so = state_.ship_orders[ship_id];
  if (so.queue.empty()) return false;
  if (!so.repeat) {
    so.repeat_count_remaining = -1;
  }
  so.repeat = true;
  so.repeat_template = so.queue;
  return true;
}

bool Simulation::disable_order_repeat(Id ship_id) {
  if (!find_ptr(state_.ships, ship_id)) return false;
  auto& so = state_.ship_orders[ship_id];
  so.repeat = false;
  so.repeat_count_remaining = 0;
  so.repeat_template.clear();
  return true;
}

bool Simulation::stop_order_repeat_keep_template(Id ship_id) {
  if (!find_ptr(state_.ships, ship_id)) return false;
  auto& so = state_.ship_orders[ship_id];
  so.repeat = false;
  so.repeat_count_remaining = 0;
  return true;
}

bool Simulation::set_order_repeat_count(Id ship_id, int repeat_count_remaining) {
  if (!find_ptr(state_.ships, ship_id)) return false;
  auto& so = state_.ship_orders[ship_id];
  if (repeat_count_remaining < -1) repeat_count_remaining = -1;
  so.repeat_count_remaining = repeat_count_remaining;
  return true;
}

bool Simulation::enable_order_repeat_from_template(Id ship_id, int repeat_count_remaining) {
  if (!find_ptr(state_.ships, ship_id)) return false;
  auto& so = state_.ship_orders[ship_id];
  if (so.repeat_template.empty()) return false;

  so.repeat = true;
  if (repeat_count_remaining < -1) repeat_count_remaining = -1;
  so.repeat_count_remaining = repeat_count_remaining;

  if (so.queue.empty()) {
    // Immediately start a cycle.
    so.queue = so.repeat_template;
  }
  return true;
}

bool Simulation::cancel_current_order(Id ship_id) {
  if (!find_ptr(state_.ships, ship_id)) return false;
  auto it = state_.ship_orders.find(ship_id);
  if (it == state_.ship_orders.end() || it->second.queue.empty()) return false;
  it->second.queue.erase(it->second.queue.begin());
  return true;
}

bool Simulation::delete_queued_order(Id ship_id, int index) {
  if (!find_ptr(state_.ships, ship_id)) return false;
  auto it = state_.ship_orders.find(ship_id);
  if (it == state_.ship_orders.end()) return false;
  auto& q = it->second.queue;
  if (index < 0 || index >= static_cast<int>(q.size())) return false;
  q.erase(q.begin() + index);
  return true;
}

bool Simulation::duplicate_queued_order(Id ship_id, int index) {
  if (!find_ptr(state_.ships, ship_id)) return false;
  auto it = state_.ship_orders.find(ship_id);
  if (it == state_.ship_orders.end()) return false;
  auto& q = it->second.queue;
  if (index < 0 || index >= static_cast<int>(q.size())) return false;
  const Order copy = q[index];
  q.insert(q.begin() + index + 1, copy);
  return true;
}

bool Simulation::move_queued_order(Id ship_id, int from_index, int to_index) {
  if (!find_ptr(state_.ships, ship_id)) return false;
  auto it = state_.ship_orders.find(ship_id);
  if (it == state_.ship_orders.end()) return false;

  auto& q = it->second.queue;
  const int n = static_cast<int>(q.size());
  if (from_index < 0 || from_index >= n) return false;

  // Interpret to_index as the desired final index after the move.
  // Allow callers to pass n (or larger) to mean "move to end".
  to_index = std::max(0, std::min(to_index, n));
  if (to_index >= n) to_index = n - 1;
  if (from_index == to_index) return true;

  Order moved = q[from_index];
  q.erase(q.begin() + from_index);

  // Insert at the desired index in the reduced vector. insert() allows index == size (end).
  to_index = std::max(0, std::min(to_index, static_cast<int>(q.size())));
  q.insert(q.begin() + to_index, std::move(moved));
  return true;
}


// --- Colony production queue editing (UI convenience) ---

bool Simulation::delete_shipyard_order(Id colony_id, int index) {
  auto* colony = find_ptr(state_.colonies, colony_id);
  if (!colony) return false;
  auto& q = colony->shipyard_queue;
  if (index < 0 || index >= static_cast<int>(q.size())) return false;
  q.erase(q.begin() + index);
  return true;
}

bool Simulation::move_shipyard_order(Id colony_id, int from_index, int to_index) {
  auto* colony = find_ptr(state_.colonies, colony_id);
  if (!colony) return false;
  auto& q = colony->shipyard_queue;
  const int n = static_cast<int>(q.size());
  if (from_index < 0 || from_index >= n) return false;

  // Interpret to_index as the desired final index after the move.
  // Allow callers to pass n (or larger) to mean "move to end".
  to_index = std::max(0, std::min(to_index, n));
  if (to_index >= n) to_index = n - 1;
  if (from_index == to_index) return true;

  BuildOrder moved = q[from_index];
  q.erase(q.begin() + from_index);

  to_index = std::max(0, std::min(to_index, static_cast<int>(q.size())));
  q.insert(q.begin() + to_index, std::move(moved));
  return true;
}

bool Simulation::delete_construction_order(Id colony_id, int index, bool refund_minerals) {
  auto* colony = find_ptr(state_.colonies, colony_id);
  if (!colony) return false;
  auto& q = colony->construction_queue;
  if (index < 0 || index >= static_cast<int>(q.size())) return false;

  if (refund_minerals) {
    const auto& ord = q[static_cast<std::size_t>(index)];
    if (ord.minerals_paid && !ord.installation_id.empty()) {
      auto it = content_.installations.find(ord.installation_id);
      if (it != content_.installations.end()) {
        for (const auto& [mineral, cost] : it->second.build_costs) {
          if (cost <= 0.0) continue;
          colony->minerals[mineral] += cost;
        }
      }
    }
  }

  q.erase(q.begin() + index);
  return true;
}

bool Simulation::move_construction_order(Id colony_id, int from_index, int to_index) {
  auto* colony = find_ptr(state_.colonies, colony_id);
  if (!colony) return false;
  auto& q = colony->construction_queue;
  const int n = static_cast<int>(q.size());
  if (from_index < 0 || from_index >= n) return false;

  // Interpret to_index as the desired final index after the move.
  // Allow callers to pass n (or larger) to mean "move to end".
  to_index = std::max(0, std::min(to_index, n));
  if (to_index >= n) to_index = n - 1;
  if (from_index == to_index) return true;

  InstallationBuildOrder moved = q[from_index];
  q.erase(q.begin() + from_index);

  to_index = std::max(0, std::min(to_index, static_cast<int>(q.size())));
  q.insert(q.begin() + to_index, std::move(moved));
  return true;
}

namespace {

bool has_non_whitespace(const std::string& s) {
  return std::any_of(s.begin(), s.end(), [](unsigned char c) { return !std::isspace(c); });
}

} // namespace

bool Simulation::save_order_template(const std::string& name, const std::vector<Order>& orders,
                                    bool overwrite, std::string* error) {
  auto fail = [&](const std::string& msg) {
    if (error) *error = msg;
    return false;
  };

  if (!has_non_whitespace(name)) return fail("Template name cannot be empty");
  if (orders.empty()) return fail("Template orders cannot be empty");

  const bool exists = (state_.order_templates.find(name) != state_.order_templates.end());
  if (exists && !overwrite) return fail("Template already exists");

  state_.order_templates[name] = orders;
  return true;
}

bool Simulation::delete_order_template(const std::string& name) {
  return state_.order_templates.erase(name) > 0;
}

bool Simulation::rename_order_template(const std::string& old_name, const std::string& new_name,
                                       std::string* error) {
  auto fail = [&](const std::string& msg) {
    if (error) *error = msg;
    return false;
  };

  if (old_name == new_name) return true;
  if (!has_non_whitespace(old_name)) return fail("Old name cannot be empty");
  if (!has_non_whitespace(new_name)) return fail("New name cannot be empty");

  auto it = state_.order_templates.find(old_name);
  if (it == state_.order_templates.end()) return fail("Template not found");
  if (state_.order_templates.find(new_name) != state_.order_templates.end()) {
    return fail("A template with that name already exists");
  }

  state_.order_templates[new_name] = std::move(it->second);
  state_.order_templates.erase(it);
  return true;
}

const std::vector<Order>* Simulation::find_order_template(const std::string& name) const {
  auto it = state_.order_templates.find(name);
  if (it == state_.order_templates.end()) return nullptr;
  return &it->second;
}

std::vector<std::string> Simulation::order_template_names() const {
  std::vector<std::string> out;
  out.reserve(state_.order_templates.size());
  for (const auto& [k, _] : state_.order_templates) out.push_back(k);
  std::sort(out.begin(), out.end());
  return out;
}

bool Simulation::apply_order_template_to_ship(Id ship_id, const std::string& name, bool append) {
  const auto* tmpl = find_order_template(name);
  if (!tmpl) return false;
  if (!find_ptr(state_.ships, ship_id)) return false;

  if (!append) {
    clear_orders(ship_id);
  }

  auto& so = state_.ship_orders[ship_id];
  so.queue.insert(so.queue.end(), tmpl->begin(), tmpl->end());
  return true;
}

bool Simulation::apply_order_template_to_fleet(Id fleet_id, const std::string& name, bool append) {
  prune_fleets();
  const auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return false;

  bool ok = true;
  for (Id sid : fl->ship_ids) {
    if (!apply_order_template_to_ship(sid, name, append)) ok = false;
  }
  return ok;
}

bool Simulation::apply_order_template_to_ship_smart(Id ship_id, const std::string& name, bool append,
                                                    bool restrict_to_discovered, std::string* error) {
  auto fail = [&](const std::string& msg) {
    if (error) *error = msg;
    return false;
  };

  const auto* tmpl = find_order_template(name);
  if (!tmpl) return fail("Template not found");

  const auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return fail("Ship not found");

  // Start from the ship's predicted system after any queued jumps if we are appending.
  PredictedNavState nav = predicted_nav_state_after_queued_jumps(state_, ship_id, append);
  if (nav.system_id == kInvalidId) return fail("Invalid ship navigation state");

  std::vector<Order> compiled;
  compiled.reserve(tmpl->size() + 8);

  auto route_to_system = [&](Id required_system_id) {
    if (required_system_id == kInvalidId) return fail("Invalid required system id");
    if (required_system_id == nav.system_id) return true;

    const auto plan = plan_jump_route_cached(nav.system_id, nav.position_mkm, ship->faction_id,
                                             ship->speed_km_s, required_system_id, restrict_to_discovered);
    if (!plan) {
      return fail("No jump route available to required system");
    }

    // Enqueue the source-side jump ids and update predicted nav state.
    for (Id jid : plan->jump_ids) {
      const auto* jp = find_ptr(state_.jump_points, jid);
      if (!jp) return fail("Route contained an invalid jump point");
      if (jp->system_id != nav.system_id) return fail("Route jump point is not in the current predicted system");
      if (jp->linked_jump_id == kInvalidId) return fail("Route jump point is unlinked");
      const auto* dest = find_ptr(state_.jump_points, jp->linked_jump_id);
      if (!dest) return fail("Route jump point has invalid destination");
      if (dest->system_id == kInvalidId) return fail("Route jump point has invalid destination system");
      if (!find_ptr(state_.systems, dest->system_id)) return fail("Route destination system does not exist");

      compiled.push_back(TravelViaJump{jid});
      nav.system_id = dest->system_id;
      nav.position_mkm = dest->position_mkm;
    }

    return true;
  };

  // Compile the template into a queue, injecting any missing travel.

  // Helper to validate a body exists and returns its system.
  auto body_system = [&](Id body_id) -> std::optional<Id> {
    const auto* b = find_ptr(state_.bodies, body_id);
    if (!b) return std::nullopt;
    if (b->system_id == kInvalidId) return std::nullopt;
    if (!find_ptr(state_.systems, b->system_id)) return std::nullopt;
    return b->system_id;
  };

  // Helper to find the system of a colony (via its body).
  auto colony_system = [&](Id colony_id) -> std::optional<Id> {
    const auto* c = find_ptr(state_.colonies, colony_id);
    if (!c) return std::nullopt;
    return body_system(c->body_id);
  };

  auto ship_system = [&](Id target_ship_id) -> std::optional<Id> {
    const auto* sh = find_ptr(state_.ships, target_ship_id);
    if (!sh) return std::nullopt;
    if (sh->system_id == kInvalidId) return std::nullopt;
    if (!find_ptr(state_.systems, sh->system_id)) return std::nullopt;
    return sh->system_id;
  };

  auto update_position_to_body = [&](Id body_id) {
    if (const auto* b = find_ptr(state_.bodies, body_id)) {
      if (b->system_id == nav.system_id) {
        nav.position_mkm = b->position_mkm;
      }
    }
  };

  auto update_position_to_colony = [&](Id colony_id) {
    const auto* c = find_ptr(state_.colonies, colony_id);
    if (!c) return;
    update_position_to_body(c->body_id);
  };

  for (const auto& ord : *tmpl) {
    // Figure out which system the ship must be in for this order to be valid.
    std::optional<Id> required_system;

    if (std::holds_alternative<MoveToBody>(ord)) {
      required_system = body_system(std::get<MoveToBody>(ord).body_id);
      if (!required_system) return fail("Template MoveToBody references an invalid body");
    } else if (std::holds_alternative<ColonizeBody>(ord)) {
      required_system = body_system(std::get<ColonizeBody>(ord).body_id);
      if (!required_system) return fail("Template ColonizeBody references an invalid body");
    } else if (std::holds_alternative<OrbitBody>(ord)) {
      required_system = body_system(std::get<OrbitBody>(ord).body_id);
      if (!required_system) return fail("Template OrbitBody references an invalid body");
    } else if (std::holds_alternative<LoadMineral>(ord)) {
      required_system = colony_system(std::get<LoadMineral>(ord).colony_id);
      if (!required_system) return fail("Template LoadMineral references an invalid colony");
    } else if (std::holds_alternative<UnloadMineral>(ord)) {
      required_system = colony_system(std::get<UnloadMineral>(ord).colony_id);
      if (!required_system) return fail("Template UnloadMineral references an invalid colony");
    } else if (std::holds_alternative<LoadTroops>(ord)) {
      required_system = colony_system(std::get<LoadTroops>(ord).colony_id);
      if (!required_system) return fail("Template LoadTroops references an invalid colony");
    } else if (std::holds_alternative<UnloadTroops>(ord)) {
      required_system = colony_system(std::get<UnloadTroops>(ord).colony_id);
      if (!required_system) return fail("Template UnloadTroops references an invalid colony");
    } else if (std::holds_alternative<InvadeColony>(ord)) {
      required_system = colony_system(std::get<InvadeColony>(ord).colony_id);
      if (!required_system) return fail("Template InvadeColony references an invalid colony");
    } else if (std::holds_alternative<ScrapShip>(ord)) {
      required_system = colony_system(std::get<ScrapShip>(ord).colony_id);
      if (!required_system) return fail("Template ScrapShip references an invalid colony");
    } else if (std::holds_alternative<AttackShip>(ord)) {
      required_system = ship_system(std::get<AttackShip>(ord).target_ship_id);
      if (!required_system) return fail("Template AttackShip references an invalid target ship");
    } else if (std::holds_alternative<TransferCargoToShip>(ord)) {
      required_system = ship_system(std::get<TransferCargoToShip>(ord).target_ship_id);
      if (!required_system) return fail("Template TransferCargoToShip references an invalid target ship");
    } else if (std::holds_alternative<TravelViaJump>(ord)) {
      const Id jid = std::get<TravelViaJump>(ord).jump_point_id;
      const auto* jp = find_ptr(state_.jump_points, jid);
      if (!jp) return fail("Template TravelViaJump references an invalid jump point");
      required_system = jp->system_id;
      if (!required_system || *required_system == kInvalidId) {
        return fail("Template TravelViaJump has an invalid source system");
      }
    }

    if (required_system) {
      if (!route_to_system(*required_system)) return false;
    }

    // Enqueue the actual template order.
    compiled.push_back(ord);

    // Update predicted nav state based on the order.
    if (std::holds_alternative<MoveToPoint>(ord)) {
      nav.position_mkm = std::get<MoveToPoint>(ord).target_mkm;
    } else if (std::holds_alternative<MoveToBody>(ord)) {
      update_position_to_body(std::get<MoveToBody>(ord).body_id);
    } else if (std::holds_alternative<ColonizeBody>(ord)) {
      update_position_to_body(std::get<ColonizeBody>(ord).body_id);
    } else if (std::holds_alternative<OrbitBody>(ord)) {
      update_position_to_body(std::get<OrbitBody>(ord).body_id);
    } else if (std::holds_alternative<LoadMineral>(ord)) {
      update_position_to_colony(std::get<LoadMineral>(ord).colony_id);
    } else if (std::holds_alternative<UnloadMineral>(ord)) {
      update_position_to_colony(std::get<UnloadMineral>(ord).colony_id);
    } else if (std::holds_alternative<LoadTroops>(ord)) {
      update_position_to_colony(std::get<LoadTroops>(ord).colony_id);
    } else if (std::holds_alternative<UnloadTroops>(ord)) {
      update_position_to_colony(std::get<UnloadTroops>(ord).colony_id);
    } else if (std::holds_alternative<InvadeColony>(ord)) {
      update_position_to_colony(std::get<InvadeColony>(ord).colony_id);
    } else if (std::holds_alternative<ScrapShip>(ord)) {
      update_position_to_colony(std::get<ScrapShip>(ord).colony_id);
      // Scrapping removes the ship; any subsequent orders would be meaningless.
      break;
    } else if (std::holds_alternative<TravelViaJump>(ord)) {
      const Id jid = std::get<TravelViaJump>(ord).jump_point_id;
      const auto* jp = find_ptr(state_.jump_points, jid);
      if (!jp) return fail("Template TravelViaJump references an invalid jump point");
      if (jp->system_id != nav.system_id) {
        // nav.system_id should already match required_system.
        return fail("Template TravelViaJump is not in the predicted system after routing");
      }
      if (jp->linked_jump_id == kInvalidId) return fail("Template TravelViaJump uses an unlinked jump point");
      const auto* dest = find_ptr(state_.jump_points, jp->linked_jump_id);
      if (!dest) return fail("Template TravelViaJump has invalid destination");
      if (dest->system_id == kInvalidId) return fail("Template TravelViaJump has invalid destination system");
      if (!find_ptr(state_.systems, dest->system_id)) return fail("Template TravelViaJump destination system missing");
      nav.system_id = dest->system_id;
      nav.position_mkm = dest->position_mkm;
    } else if (std::holds_alternative<AttackShip>(ord)) {
      // Best-effort: update position to the current target snapshot if it's in the same system.
      const Id tid = std::get<AttackShip>(ord).target_ship_id;
      if (const auto* t = find_ptr(state_.ships, tid)) {
        if (t->system_id == nav.system_id) nav.position_mkm = t->position_mkm;
      }
    } else if (std::holds_alternative<TransferCargoToShip>(ord)) {
      const Id tid = std::get<TransferCargoToShip>(ord).target_ship_id;
      if (const auto* t = find_ptr(state_.ships, tid)) {
        if (t->system_id == nav.system_id) nav.position_mkm = t->position_mkm;
      }
    }
  }

  if (compiled.empty()) return fail("Template produced no orders");

  // Apply atomically after successful compilation.
  if (!append) {
    if (!clear_orders(ship_id)) return fail("Failed to clear orders");
  }

  auto& so = state_.ship_orders[ship_id];
  so.queue.insert(so.queue.end(), compiled.begin(), compiled.end());
  return true;
}

bool Simulation::apply_order_template_to_fleet_smart(Id fleet_id, const std::string& name, bool append,
                                                     bool restrict_to_discovered, std::string* error) {
  prune_fleets();
  const auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) {
    if (error) *error = "Fleet not found";
    return false;
  }

  bool ok_any = false;
  std::string last_err;
  for (Id sid : fl->ship_ids) {
    std::string err;
    if (apply_order_template_to_ship_smart(sid, name, append, restrict_to_discovered, &err)) {
      ok_any = true;
    } else {
      last_err = err;
    }
  }

  if (!ok_any && error) *error = last_err;
  return ok_any;
}

Id Simulation::create_fleet(Id faction_id, const std::string& name, const std::vector<Id>& ship_ids,
                            std::string* error) {
  prune_fleets();

  auto fail = [&](const std::string& msg) -> Id {
    if (error) *error = msg;
    return kInvalidId;
  };

  if (faction_id == kInvalidId) return fail("Invalid faction id");
  if (!find_ptr(state_.factions, faction_id)) return fail("Faction does not exist");
  if (ship_ids.empty()) return fail("No ships provided");

  std::vector<Id> members;
  members.reserve(ship_ids.size());

  for (Id sid : ship_ids) {
    if (sid == kInvalidId) return fail("Invalid ship id in list");
    const auto* sh = find_ptr(state_.ships, sid);
    if (!sh) return fail("Ship does not exist: " + std::to_string(static_cast<unsigned long long>(sid)));
    if (sh->faction_id != faction_id) {
      return fail("Ship belongs to a different faction: " + sh->name);
    }
    const Id existing = fleet_for_ship(sid);
    if (existing != kInvalidId) {
      return fail("Ship already belongs to fleet " + std::to_string(static_cast<unsigned long long>(existing)));
    }
    members.push_back(sid);
  }

  std::sort(members.begin(), members.end());
  members.erase(std::unique(members.begin(), members.end()), members.end());
  if (members.empty()) return fail("No valid ships provided");

  Fleet fl;
  fl.id = allocate_id(state_);
  fl.name = name.empty() ? ("Fleet " + std::to_string(static_cast<unsigned long long>(fl.id))) : name;
  fl.faction_id = faction_id;
  fl.ship_ids = members;
  fl.leader_ship_id = members.front();

  const Id fleet_id = fl.id;
  state_.fleets[fleet_id] = std::move(fl);
  return fleet_id;
}

bool Simulation::disband_fleet(Id fleet_id) {
  return state_.fleets.erase(fleet_id) > 0;
}

bool Simulation::add_ship_to_fleet(Id fleet_id, Id ship_id, std::string* error) {
  prune_fleets();

  auto fail = [&](const std::string& msg) {
    if (error) *error = msg;
    return false;
  };

  auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return fail("Fleet does not exist");

  if (ship_id == kInvalidId) return fail("Invalid ship id");
  const auto* sh = find_ptr(state_.ships, ship_id);
  if (!sh) return fail("Ship does not exist");
  if (fl->faction_id != kInvalidId && sh->faction_id != fl->faction_id) {
    return fail("Ship faction does not match fleet faction");
  }

  const Id existing = fleet_for_ship(ship_id);
  if (existing != kInvalidId && existing != fleet_id) {
    return fail("Ship already belongs to fleet " + std::to_string(static_cast<unsigned long long>(existing)));
  }

  if (std::find(fl->ship_ids.begin(), fl->ship_ids.end(), ship_id) != fl->ship_ids.end()) {
    return true; // already in this fleet
  }

  fl->ship_ids.push_back(ship_id);
  std::sort(fl->ship_ids.begin(), fl->ship_ids.end());
  fl->ship_ids.erase(std::unique(fl->ship_ids.begin(), fl->ship_ids.end()), fl->ship_ids.end());
  if (fl->leader_ship_id == kInvalidId && !fl->ship_ids.empty()) fl->leader_ship_id = fl->ship_ids.front();
  return true;
}

bool Simulation::remove_ship_from_fleet(Id fleet_id, Id ship_id) {
  auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return false;

  const auto it = std::remove(fl->ship_ids.begin(), fl->ship_ids.end(), ship_id);
  if (it == fl->ship_ids.end()) return false;
  fl->ship_ids.erase(it, fl->ship_ids.end());
  if (fl->leader_ship_id == ship_id) fl->leader_ship_id = kInvalidId;
  prune_fleets();
  return true;
}

bool Simulation::set_fleet_leader(Id fleet_id, Id ship_id) {
  auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return false;
  if (ship_id == kInvalidId) return false;
  if (std::find(fl->ship_ids.begin(), fl->ship_ids.end(), ship_id) == fl->ship_ids.end()) return false;
  fl->leader_ship_id = ship_id;
  return true;
}

bool Simulation::rename_fleet(Id fleet_id, const std::string& name) {
  auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return false;
  if (name.empty()) return false;
  fl->name = name;
  return true;
}

bool Simulation::configure_fleet_formation(Id fleet_id, FleetFormation formation, double spacing_mkm) {
  auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return false;
  fl->formation = formation;
  fl->formation_spacing_mkm = std::max(0.0, spacing_mkm);
  return true;
}

Id Simulation::fleet_for_ship(Id ship_id) const {
  if (ship_id == kInvalidId) return kInvalidId;
  for (const auto& [fid, fl] : state_.fleets) {
    if (std::find(fl.ship_ids.begin(), fl.ship_ids.end(), ship_id) != fl.ship_ids.end()) return fid;
  }
  return kInvalidId;
}

bool Simulation::clear_fleet_orders(Id fleet_id) {
  prune_fleets();
  const auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return false;

  bool any = false;
  for (Id sid : fl->ship_ids) {
    if (clear_orders(sid)) any = true;
  }
  return any;
}

bool Simulation::issue_fleet_wait_days(Id fleet_id, int days) {
  prune_fleets();
  const auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return false;
  bool any = false;
  for (Id sid : fl->ship_ids) {
    if (issue_wait_days(sid, days)) any = true;
  }
  return any;
}

bool Simulation::issue_fleet_move_to_point(Id fleet_id, Vec2 target_mkm) {
  prune_fleets();
  const auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return false;
  bool any = false;
  for (Id sid : fl->ship_ids) {
    if (issue_move_to_point(sid, target_mkm)) any = true;
  }
  return any;
}

bool Simulation::issue_fleet_move_to_body(Id fleet_id, Id body_id, bool restrict_to_discovered) {
  prune_fleets();
  const auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return false;
  bool any = false;
  for (Id sid : fl->ship_ids) {
    if (issue_move_to_body(sid, body_id, restrict_to_discovered)) any = true;
  }
  return any;
}

bool Simulation::issue_fleet_orbit_body(Id fleet_id, Id body_id, int duration_days, bool restrict_to_discovered) {
  prune_fleets();
  const auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return false;
  bool any = false;
  for (Id sid : fl->ship_ids) {
    if (issue_orbit_body(sid, body_id, duration_days, restrict_to_discovered)) any = true;
  }
  return any;
}

bool Simulation::issue_fleet_travel_via_jump(Id fleet_id, Id jump_point_id) {
  prune_fleets();
  const auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return false;
  bool any = false;
  for (Id sid : fl->ship_ids) {
    if (issue_travel_via_jump(sid, jump_point_id)) any = true;
  }
  return any;
}

bool Simulation::issue_fleet_travel_to_system(Id fleet_id, Id target_system_id, bool restrict_to_discovered) {
  prune_fleets();
  const auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return false;

  if (!find_ptr(state_.systems, target_system_id)) return false;
  if (fl->ship_ids.empty()) return false;

  // Prefer routing once for the whole fleet so every ship takes the same hop sequence.
  // If ships are not co-located (after their queued jumps), fall back to per-ship routing.
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

  if (!leader) return false;

  const PredictedNavState leader_nav = predicted_nav_state_after_queued_jumps(state_, leader_id,
                                                                              /*include_queued_jumps=*/true);
  if (leader_nav.system_id == kInvalidId) return false;

  bool colocated = true;
  for (Id sid : fl->ship_ids) {
    const PredictedNavState nav = predicted_nav_state_after_queued_jumps(state_, sid, /*include_queued_jumps=*/true);
    if (nav.system_id != leader_nav.system_id) {
      colocated = false;
      break;
    }
  }

  if (!colocated) {
    bool any = false;
    for (Id sid : fl->ship_ids) {
      if (issue_travel_to_system(sid, target_system_id, restrict_to_discovered)) any = true;
    }
    return any;
  }

  if (leader_nav.system_id == target_system_id) return true; // no-op

  const auto plan = plan_jump_route_cached(leader_nav.system_id, leader_nav.position_mkm,
                                          fl->faction_id, leader->speed_km_s, target_system_id,
                                          restrict_to_discovered);
  if (!plan) return false;

  bool any = false;
  for (Id sid : fl->ship_ids) {
    if (!find_ptr(state_.ships, sid)) continue;
    auto& orders = state_.ship_orders[sid];
    for (Id jid : plan->jump_ids) orders.queue.push_back(TravelViaJump{jid});
    any = true;
  }
  return any;
}

bool Simulation::issue_fleet_attack_ship(Id fleet_id, Id target_ship_id, bool restrict_to_discovered) {
  prune_fleets();
  const auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return false;
  bool any = false;
  for (Id sid : fl->ship_ids) {
    if (issue_attack_ship(sid, target_ship_id, restrict_to_discovered)) any = true;
  }
  return any;
}

bool Simulation::issue_fleet_load_mineral(Id fleet_id, Id colony_id, const std::string& mineral, double tons,
                                          bool restrict_to_discovered) {
  prune_fleets();
  const auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return false;
  bool any = false;
  for (Id sid : fl->ship_ids) {
    if (issue_load_mineral(sid, colony_id, mineral, tons, restrict_to_discovered)) any = true;
  }
  return any;
}

bool Simulation::issue_fleet_unload_mineral(Id fleet_id, Id colony_id, const std::string& mineral, double tons,
                                            bool restrict_to_discovered) {
  prune_fleets();
  const auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return false;
  bool any = false;
  for (Id sid : fl->ship_ids) {
    if (issue_unload_mineral(sid, colony_id, mineral, tons, restrict_to_discovered)) any = true;
  }
  return any;
}

bool Simulation::issue_fleet_transfer_cargo_to_ship(Id fleet_id, Id target_ship_id, const std::string& mineral,
                                                    double tons, bool restrict_to_discovered) {
  prune_fleets();
  const auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return false;
  bool any = false;
  for (Id sid : fl->ship_ids) {
    if (issue_transfer_cargo_to_ship(sid, target_ship_id, mineral, tons, restrict_to_discovered)) any = true;
  }
  return any;
}

bool Simulation::issue_fleet_scrap_ship(Id fleet_id, Id colony_id, bool restrict_to_discovered) {
  prune_fleets();
  const auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return false;
  bool any = false;
  for (Id sid : fl->ship_ids) {
    if (issue_scrap_ship(sid, colony_id, restrict_to_discovered)) any = true;
  }
  return any;
}

bool Simulation::issue_wait_days(Id ship_id, int days) {
  if (days <= 0) return false;
  if (!find_ptr(state_.ships, ship_id)) return false;
  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(WaitDays{days});
  return true;
}

bool Simulation::issue_move_to_point(Id ship_id, Vec2 target_mkm) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(MoveToPoint{target_mkm});
  return true;
}

bool Simulation::issue_move_to_body(Id ship_id, Id body_id, bool restrict_to_discovered) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  const auto* body = find_ptr(state_.bodies, body_id);
  if (!body) return false;

  const Id target_system_id = body->system_id;
  if (target_system_id == kInvalidId) return false;
  if (!find_ptr(state_.systems, target_system_id)) return false;

  if (!issue_travel_to_system(ship_id, target_system_id, restrict_to_discovered)) return false;

  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(MoveToBody{body_id});
  return true;
}

bool Simulation::issue_colonize_body(Id ship_id, Id body_id, const std::string& colony_name,
                                    bool restrict_to_discovered) {
  const auto* body = find_ptr(state_.bodies, body_id);
  if (!body) return false;

  // Route across the jump network if needed.
  if (!issue_travel_to_system(ship_id, body->system_id, restrict_to_discovered)) return false;

  auto& q = state_.ship_orders[ship_id].queue;
  ColonizeBody ord;
  ord.body_id = body_id;
  ord.colony_name = colony_name;
  q.push_back(std::move(ord));
  return true;
}

bool Simulation::issue_orbit_body(Id ship_id, Id body_id, int duration_days, bool restrict_to_discovered) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  const auto* body = find_ptr(state_.bodies, body_id);
  if (!body) return false;
  
  const Id target_system_id = body->system_id;
  if (target_system_id == kInvalidId) return false;
  if (!find_ptr(state_.systems, target_system_id)) return false;

  if (!issue_travel_to_system(ship_id, target_system_id, restrict_to_discovered)) return false;

  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(OrbitBody{body_id, duration_days});
  return true;
}

bool Simulation::issue_travel_via_jump(Id ship_id, Id jump_point_id) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  if (!find_ptr(state_.jump_points, jump_point_id)) return false;
  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(TravelViaJump{jump_point_id});
  return true;
}

bool Simulation::issue_travel_to_system(Id ship_id, Id target_system_id, bool restrict_to_discovered) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  if (!find_ptr(state_.systems, target_system_id)) return false;

  const PredictedNavState nav = predicted_nav_state_after_queued_jumps(state_, ship_id, /*include_queued_jumps=*/true);
  if (nav.system_id == kInvalidId) return false;
  if (nav.system_id == target_system_id) return true; // no-op

  const auto plan = plan_jump_route_cached(nav.system_id, nav.position_mkm, ship->faction_id, ship->speed_km_s,
                                          target_system_id, restrict_to_discovered);
  if (!plan) return false;

  auto& orders = state_.ship_orders[ship_id];
  for (Id jid : plan->jump_ids) orders.queue.push_back(TravelViaJump{jid});
  return true;
}

bool Simulation::issue_attack_ship(Id attacker_ship_id, Id target_ship_id, bool restrict_to_discovered) {
  if (attacker_ship_id == target_ship_id) return false;
  auto* attacker = find_ptr(state_.ships, attacker_ship_id);
  if (!attacker) return false;
  const auto* target = find_ptr(state_.ships, target_ship_id);
  if (!target) return false;
  if (target->faction_id == attacker->faction_id) return false;

  const bool detected = is_ship_detected_by_faction(attacker->faction_id, target_ship_id);

  AttackShip ord;
  ord.target_ship_id = target_ship_id;

  Id target_system_id = kInvalidId;

  if (detected) {
    ord.has_last_known = true;
    ord.last_known_position_mkm = target->position_mkm;
    target_system_id = target->system_id;
  } else {
    const auto* fac = find_ptr(state_.factions, attacker->faction_id);
    if (!fac) return false;
    const auto it = fac->ship_contacts.find(target_ship_id);
    if (it == fac->ship_contacts.end()) return false;
    ord.has_last_known = true;
    ord.last_known_position_mkm = it->second.last_seen_position_mkm;
    target_system_id = it->second.system_id;
  }

  if (target_system_id == kInvalidId) return false;
  if (!find_ptr(state_.systems, target_system_id)) return false;

  if (!issue_travel_to_system(attacker_ship_id, target_system_id, restrict_to_discovered)) return false;

  auto& orders = state_.ship_orders[attacker_ship_id];
  orders.queue.push_back(ord);
  return true;
}

bool Simulation::issue_load_mineral(Id ship_id, Id colony_id, const std::string& mineral, double tons,
                                    bool restrict_to_discovered) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  auto* colony = find_ptr(state_.colonies, colony_id);
  if (!colony) return false;
  if (colony->faction_id != ship->faction_id) return false;
  const auto* body = find_ptr(state_.bodies, colony->body_id);
  if (!body) return false;
  if (body->system_id == kInvalidId) return false;
  if (!find_ptr(state_.systems, body->system_id)) return false;
  if (tons < 0.0) return false;

  if (!issue_travel_to_system(ship_id, body->system_id, restrict_to_discovered)) return false;

  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(LoadMineral{colony_id, mineral, tons});
  return true;
}

bool Simulation::issue_unload_mineral(Id ship_id, Id colony_id, const std::string& mineral, double tons,
                                      bool restrict_to_discovered) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  auto* colony = find_ptr(state_.colonies, colony_id);
  if (!colony) return false;
  if (colony->faction_id != ship->faction_id) return false;
  const auto* body = find_ptr(state_.bodies, colony->body_id);
  if (!body) return false;
  if (body->system_id == kInvalidId) return false;
  if (!find_ptr(state_.systems, body->system_id)) return false;
  if (tons < 0.0) return false;

  if (!issue_travel_to_system(ship_id, body->system_id, restrict_to_discovered)) return false;

  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(UnloadMineral{colony_id, mineral, tons});
  return true;
}

bool Simulation::issue_load_troops(Id ship_id, Id colony_id, double strength, bool restrict_to_discovered) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  auto* colony = find_ptr(state_.colonies, colony_id);
  if (!colony) return false;
  if (colony->faction_id != ship->faction_id) return false;
  const auto* body = find_ptr(state_.bodies, colony->body_id);
  if (!body) return false;
  if (body->system_id == kInvalidId) return false;
  if (!find_ptr(state_.systems, body->system_id)) return false;
  if (strength < 0.0) return false;

  if (!issue_travel_to_system(ship_id, body->system_id, restrict_to_discovered)) return false;

  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(LoadTroops{colony_id, strength});
  return true;
}

bool Simulation::issue_unload_troops(Id ship_id, Id colony_id, double strength, bool restrict_to_discovered) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  auto* colony = find_ptr(state_.colonies, colony_id);
  if (!colony) return false;
  if (colony->faction_id != ship->faction_id) return false;
  const auto* body = find_ptr(state_.bodies, colony->body_id);
  if (!body) return false;
  if (body->system_id == kInvalidId) return false;
  if (!find_ptr(state_.systems, body->system_id)) return false;
  if (strength < 0.0) return false;

  if (!issue_travel_to_system(ship_id, body->system_id, restrict_to_discovered)) return false;

  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(UnloadTroops{colony_id, strength});
  return true;
}

bool Simulation::issue_invade_colony(Id ship_id, Id colony_id, bool restrict_to_discovered) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  auto* colony = find_ptr(state_.colonies, colony_id);
  if (!colony) return false;
  if (colony->faction_id == ship->faction_id) return false;
  const auto* body = find_ptr(state_.bodies, colony->body_id);
  if (!body) return false;
  if (body->system_id == kInvalidId) return false;
  if (!find_ptr(state_.systems, body->system_id)) return false;

  if (!issue_travel_to_system(ship_id, body->system_id, restrict_to_discovered)) return false;

  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(InvadeColony{colony_id});
  return true;
}

bool Simulation::enqueue_troop_training(Id colony_id, double strength) {
  auto* colony = find_ptr(state_.colonies, colony_id);
  if (!colony) return false;
  if (strength <= 0.0) return false;
  colony->troop_training_queue += strength;
  return true;
}

bool Simulation::clear_troop_training_queue(Id colony_id) {
  auto* colony = find_ptr(state_.colonies, colony_id);
  if (!colony) return false;
  colony->troop_training_queue = 0.0;
  return true;
}

bool Simulation::set_terraforming_target(Id body_id, double target_temp_k, double target_atm) {
  auto* body = find_ptr(state_.bodies, body_id);
  if (!body) return false;
  if (target_temp_k <= 0.0 && target_atm <= 0.0) return false;
  body->terraforming_target_temp_k = std::max(0.0, target_temp_k);
  body->terraforming_target_atm = std::max(0.0, target_atm);
  body->terraforming_complete = false;
  return true;
}

bool Simulation::clear_terraforming_target(Id body_id) {
  auto* body = find_ptr(state_.bodies, body_id);
  if (!body) return false;
  body->terraforming_target_temp_k = 0.0;
  body->terraforming_target_atm = 0.0;
  body->terraforming_complete = false;
  return true;
}

double Simulation::terraforming_points_per_day(const Colony& c) const {
  double total = 0.0;
  for (const auto& [inst_id, count] : c.installations) {
    if (count <= 0) continue;
    auto it = content_.installations.find(inst_id);
    if (it == content_.installations.end()) continue;
    const double p = it->second.terraforming_points_per_day;
    if (p > 0.0) total += p * static_cast<double>(count);
  }
  if (const auto* fac = find_ptr(state_.factions, c.faction_id)) {
    const auto m = compute_faction_economy_multipliers(content_, *fac);
    total *= std::max(0.0, m.terraforming);
  }
  return std::max(0.0, total);
}

double Simulation::troop_training_points_per_day(const Colony& c) const {
  double total = 0.0;
  for (const auto& [inst_id, count] : c.installations) {
    if (count <= 0) continue;
    auto it = content_.installations.find(inst_id);
    if (it == content_.installations.end()) continue;
    const double p = it->second.troop_training_points_per_day;
    if (p > 0.0) total += p * static_cast<double>(count);
  }
  if (const auto* fac = find_ptr(state_.factions, c.faction_id)) {
    const auto m = compute_faction_economy_multipliers(content_, *fac);
    total *= std::max(0.0, m.troop_training);
  }
  return std::max(0.0, total);
}

double Simulation::fortification_points(const Colony& c) const {
  double total = 0.0;
  for (const auto& [inst_id, count] : c.installations) {
    if (count <= 0) continue;
    auto it = content_.installations.find(inst_id);
    if (it == content_.installations.end()) continue;
    const double p = it->second.fortification_points;
    if (p > 0.0) total += p * static_cast<double>(count);
  }
  return total;
}

bool Simulation::issue_transfer_cargo_to_ship(Id ship_id, Id target_ship_id, const std::string& mineral, double tons,
                                              bool restrict_to_discovered) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  auto* target = find_ptr(state_.ships, target_ship_id);
  if (!target) return false;
  
  if (ship->faction_id != target->faction_id) return false;
  if (tons < 0.0) return false;

  if (!issue_travel_to_system(ship_id, target->system_id, restrict_to_discovered)) return false;

  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(TransferCargoToShip{target_ship_id, mineral, tons});
  return true;
}

bool Simulation::issue_scrap_ship(Id ship_id, Id colony_id, bool restrict_to_discovered) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  auto* colony = find_ptr(state_.colonies, colony_id);
  if (!colony) return false;
  if (colony->faction_id != ship->faction_id) return false;

  const auto* body = find_ptr(state_.bodies, colony->body_id);
  if (!body) return false;

  if (!issue_travel_to_system(ship_id, body->system_id, restrict_to_discovered)) return false;

  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(ScrapShip{colony_id});
  return true;
}

bool Simulation::enqueue_build(Id colony_id, const std::string& design_id) {
  auto* colony = find_ptr(state_.colonies, colony_id);
  if (!colony) return false;
  const auto it_yard = colony->installations.find("shipyard");
  if (it_yard == colony->installations.end() || it_yard->second <= 0) return false;
  const auto* d = find_design(design_id);
  if (!d) return false;
  if (!is_design_buildable_for_faction(colony->faction_id, design_id)) return false;
  BuildOrder bo;
  bo.design_id = design_id;
  bo.tons_remaining = std::max(1.0, d->mass_tons);
  colony->shipyard_queue.push_back(bo);
  return true;
}

double Simulation::estimate_refit_tons(Id ship_id, const std::string& target_design_id) const {
  const auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return 0.0;

  const auto* target = find_design(target_design_id);
  if (!target) return 0.0;

  const double mult = std::max(0.0, cfg_.ship_refit_tons_multiplier);
  return std::max(1.0, target->mass_tons * mult);
}

bool Simulation::enqueue_refit(Id colony_id, Id ship_id, const std::string& target_design_id, std::string* error) {
  auto fail = [&](const std::string& msg) {
    if (error) *error = msg;
    return false;
  };

  auto* colony = find_ptr(state_.colonies, colony_id);
  if (!colony) return fail("Colony not found");

  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return fail("Ship not found");

  if (ship->faction_id != colony->faction_id) return fail("Ship does not belong to the colony faction");

  const auto it_yard = colony->installations.find("shipyard");
  const int yards = (it_yard != colony->installations.end()) ? it_yard->second : 0;
  if (yards <= 0) return fail("Colony has no shipyard");

  const auto* target = find_design(target_design_id);
  if (!target) return fail("Unknown target design: " + target_design_id);
  if (!is_design_buildable_for_faction(colony->faction_id, target_design_id)) return fail("Target design is not unlocked");

  // Refit requires the ship to be docked at the colony at the time of queuing.
  if (!is_ship_docked_at_colony(ship_id, colony_id)) return fail("Ship is not docked at the colony");

  // Keep the prototype simple: refit ships must be detached from fleets.
  if (fleet_for_ship(ship_id) != kInvalidId) return fail("Ship is assigned to a fleet (detach before refit)");

  // Prevent duplicate queued refits for the same ship.
  for (const auto& [_, c] : state_.colonies) {
    for (const auto& bo : c.shipyard_queue) {
      if (bo.refit_ship_id == ship_id) return fail("Ship already has a pending refit order");
    }
  }

  BuildOrder bo;
  bo.design_id = target_design_id;
  bo.refit_ship_id = ship_id;
  bo.tons_remaining = estimate_refit_tons(ship_id, target_design_id);
  colony->shipyard_queue.push_back(bo);

  // Log a helpful event for the player.
  {
    EventContext ctx;
    ctx.faction_id = colony->faction_id;
    ctx.system_id = ship->system_id;
    ctx.ship_id = ship->id;
    ctx.colony_id = colony->id;

    std::string msg = "Shipyard refit queued: " + ship->name + " -> " + target->name + " at " + colony->name;
    push_event(EventLevel::Info, EventCategory::Shipyard, std::move(msg), ctx);
  }

  return true;
}

bool Simulation::enqueue_installation_build(Id colony_id, const std::string& installation_id, int quantity) {
  auto* colony = find_ptr(state_.colonies, colony_id);
  if (!colony) return false;
  if (quantity <= 0) return false;
  if (content_.installations.find(installation_id) == content_.installations.end()) return false;
  if (!is_installation_buildable_for_faction(colony->faction_id, installation_id)) return false;

  InstallationBuildOrder o;
  o.installation_id = installation_id;
  o.quantity_remaining = quantity;
  colony->construction_queue.push_back(o);
  return true;
}

void Simulation::recompute_body_positions() {
  const double t = static_cast<double>(state_.date.days_since_epoch());

  // Bodies may orbit other bodies (e.g., moons). Compute absolute positions in a
  // parent-first manner, but remain robust to unordered_map iteration order.
  std::unordered_map<Id, Vec2> cache;
  cache.reserve(state_.bodies.size() * 2);

  std::unordered_set<Id> visiting;
  visiting.reserve(state_.bodies.size());

  const auto compute_pos = [&](Id id, const auto& self) -> Vec2 {
    if (id == kInvalidId) return {0.0, 0.0};

    if (auto it = cache.find(id); it != cache.end()) return it->second;

    auto itb = state_.bodies.find(id);
    if (itb == state_.bodies.end()) return {0.0, 0.0};

    Body& b = itb->second;

    // Break accidental cycles gracefully (treat as orbiting system origin).
    if (!visiting.insert(id).second) {
      cache[id] = {0.0, 0.0};
      return {0.0, 0.0};
    }

    // Orbit center: either system origin or a parent body's current position.
    Vec2 center{0.0, 0.0};
    if (b.parent_body_id != kInvalidId && b.parent_body_id != id) {
      const Body* parent = find_ptr(state_.bodies, b.parent_body_id);
      if (parent && parent->system_id == b.system_id) {
        center = self(b.parent_body_id, self);
      }
    }

    Vec2 pos = center;
    if (b.orbit_radius_mkm > 1e-9) {
      const double a = std::max(0.0, b.orbit_radius_mkm);
      const double e = std::clamp(b.orbit_eccentricity, 0.0, 0.999999);
      const double period = std::max(1.0, b.orbit_period_days);

      // Mean anomaly advances linearly with time.
      double M = b.orbit_phase_radians + kTwoPi * (t / period);
      // Wrap for numerical stability.
      M = std::fmod(M, kTwoPi);
      if (M < 0.0) M += kTwoPi;

      // Solve Kepler's equation: M = E - e sin(E) for eccentric anomaly E.
      // Newton iteration converges quickly for typical orbital eccentricities.
      double E = (e < 0.8) ? M : (kTwoPi * 0.5); // start at pi for high-e orbits
      for (int it = 0; it < 12; ++it) {
        const double sE = std::sin(E);
        const double cE = std::cos(E);
        const double f = (E - e * sE) - M;
        const double fp = 1.0 - e * cE;
        if (std::fabs(fp) < 1e-12) break;
        const double d = f / fp;
        E -= d;
        if (std::fabs(f) < 1e-10) break;
      }

      const double sE = std::sin(E);
      const double cE = std::cos(E);
      const double bsemi = a * std::sqrt(std::max(0.0, 1.0 - e * e));
      const double x = a * (cE - e);
      const double y = bsemi * sE;

      const double w = b.orbit_arg_periapsis_radians;
      const double cw = std::cos(w);
      const double sw = std::sin(w);
      const double rx = x * cw - y * sw;
      const double ry = x * sw + y * cw;

      pos = center + Vec2{rx, ry};
    }

    cache[id] = pos;
    visiting.erase(id);
    return pos;
  };

  for (auto& [id, b] : state_.bodies) {
    b.position_mkm = compute_pos(id, compute_pos);
  }
}

void Simulation::tick_one_day() {
  state_.date = state_.date.add_days(1);
  recompute_body_positions();
  tick_colonies();
  tick_research();
  tick_shipyards();
  tick_construction();
  tick_ai();
  tick_refuel();
  tick_ships();
  tick_contacts();
  tick_shields();
  if (cfg_.enable_combat) tick_combat();
  tick_ground_combat();
  tick_terraforming();
  tick_repairs();
}

void Simulation::push_event(EventLevel level, std::string message) {
  push_event(level, EventCategory::General, std::move(message), {});
}

void Simulation::push_event(EventLevel level, EventCategory category, std::string message, EventContext ctx) {
  SimEvent ev;
  ev.seq = state_.next_event_seq;
  state_.next_event_seq += 1;
  if (state_.next_event_seq == 0) state_.next_event_seq = 1; 

  ev.day = state_.date.days_since_epoch();
  ev.level = level;
  ev.category = category;
  ev.faction_id = ctx.faction_id;
  ev.faction_id2 = ctx.faction_id2;
  ev.system_id = ctx.system_id;
  ev.ship_id = ctx.ship_id;
  ev.colony_id = ctx.colony_id;
  ev.message = std::move(message);
  state_.events.push_back(std::move(ev));

  const int max_events = cfg_.max_events;
  if (max_events > 0 && static_cast<int>(state_.events.size()) > max_events + 128) {
    const std::size_t keep = static_cast<std::size_t>(max_events);
    const std::size_t cut = state_.events.size() - keep;
    state_.events.erase(state_.events.begin(), state_.events.begin() + static_cast<std::ptrdiff_t>(cut));
  }
}

void Simulation::tick_contacts() {
  const int now = static_cast<int>(state_.date.days_since_epoch());
  constexpr int kMaxContactAgeDays = 180;

  for (auto& [_, fac] : state_.factions) {
    for (auto it = fac.ship_contacts.begin(); it != fac.ship_contacts.end();) {
      const Contact& c = it->second;
      const bool dead = (state_.ships.find(c.ship_id) == state_.ships.end());
      const int age = now - c.last_seen_day;
      if (dead || age > kMaxContactAgeDays) {
        it = fac.ship_contacts.erase(it);
      } else {
        ++it;
      }
    }
  }

  struct Key {
    Id faction_id{kInvalidId};
    Id system_id{kInvalidId};
    bool operator==(const Key& o) const { return faction_id == o.faction_id && system_id == o.system_id; }
  };
  struct KeyHash {
    size_t operator()(const Key& k) const {
      return std::hash<long long>()((static_cast<long long>(k.faction_id) << 32) ^ static_cast<long long>(k.system_id));
    }
  };

  std::unordered_map<Key, std::vector<SensorSource>, KeyHash> cache;

  auto sources_for = [&](Id faction_id, Id system_id) -> const std::vector<SensorSource>& {
    const Key key{faction_id, system_id};
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    auto sources = gather_sensor_sources(*this, faction_id, system_id);
    auto [ins, _ok] = cache.emplace(key, std::move(sources));
    return ins->second;
  };

  std::unordered_map<Id, std::vector<Id>> detected_today_by_faction;
  detected_today_by_faction.reserve(state_.factions.size());

  const auto faction_ids = sorted_keys(state_.factions);
  const auto system_ids = sorted_keys(state_.systems);

  // Build a compact list of (ship, viewer faction) detections for today.
  //
  // We later sort these pairs by (ship_id, faction_id) to preserve the exact
  // deterministic ordering used by the original nested loop over
  // sorted ship ids then sorted faction ids.
  struct DetectionPair {
    Id ship_id{kInvalidId};
    Id viewer_faction_id{kInvalidId};
  };

  std::vector<DetectionPair> detections;
  detections.reserve(std::min<std::size_t>(state_.ships.size() * 2, 4096));

  std::unordered_map<Id, SpatialIndex2D> system_index;
  system_index.reserve(state_.systems.size());

  auto index_for_system = [&](Id sys_id) -> SpatialIndex2D& {
    auto it = system_index.find(sys_id);
    if (it != system_index.end()) return it->second;
    SpatialIndex2D idx;
    if (const auto* sys = find_ptr(state_.systems, sys_id)) {
      idx.build_from_ship_ids(sys->ships, state_.ships);
    }
    auto [ins, _ok] = system_index.emplace(sys_id, std::move(idx));
    return ins->second;
  };

  for (Id sys_id : system_ids) {
    const auto* sys = find_ptr(state_.systems, sys_id);
    if (!sys) continue;
    if (sys->ships.empty()) continue;

    auto& idx = index_for_system(sys_id);

    for (Id fid : faction_ids) {
      const auto& sources = sources_for(fid, sys_id);
      if (sources.empty()) continue;

      for (const auto& src : sources) {
        if (src.range_mkm <= 1e-9) continue;

        const auto nearby = idx.query_radius(src.pos_mkm, src.range_mkm, 1e-9);
        for (Id ship_id : nearby) {
          const auto* sh = find_ptr(state_.ships, ship_id);
          if (!sh) continue;
          if (sh->system_id != sys_id) continue;
          if (sh->faction_id == fid) continue;
          detections.push_back(DetectionPair{ship_id, fid});
        }
      }
    }
  }

  std::sort(detections.begin(), detections.end(), [](const DetectionPair& a, const DetectionPair& b) {
    if (a.ship_id != b.ship_id) return a.ship_id < b.ship_id;
    return a.viewer_faction_id < b.viewer_faction_id;
  });
  detections.erase(std::unique(detections.begin(), detections.end(), [](const DetectionPair& a, const DetectionPair& b) {
    return a.ship_id == b.ship_id && a.viewer_faction_id == b.viewer_faction_id;
  }), detections.end());

  // Apply today's detections to each faction's contact list.
  for (const auto& det : detections) {
    const auto* sh = find_ptr(state_.ships, det.ship_id);
    if (!sh) continue;

    auto* fac = find_ptr(state_.factions, det.viewer_faction_id);
    if (!fac) continue;
    if (fac->id == sh->faction_id) continue;

    detected_today_by_faction[fac->id].push_back(det.ship_id);

    bool is_new = false;
    bool was_stale = false;
    if (auto it = fac->ship_contacts.find(det.ship_id); it == fac->ship_contacts.end()) {
      is_new = true;
    } else {
      was_stale = (it->second.last_seen_day < now - 1);
    }

    Contact c;
    c.ship_id = det.ship_id;
    c.system_id = sh->system_id;
    c.last_seen_day = now;
    c.last_seen_position_mkm = sh->position_mkm;
    c.last_seen_name = sh->name;
    c.last_seen_design_id = sh->design_id;
    c.last_seen_faction_id = sh->faction_id;
    fac->ship_contacts[det.ship_id] = std::move(c);

    if (is_new || was_stale) {
      const auto* sys = find_ptr(state_.systems, sh->system_id);
      const std::string sys_name = sys ? sys->name : std::string("(unknown)");
      const auto* other_f = find_ptr(state_.factions, sh->faction_id);
      const std::string other_name = other_f ? other_f->name : std::string("(unknown)");

      EventContext ctx;
      ctx.faction_id = fac->id;
      ctx.faction_id2 = sh->faction_id;
      ctx.system_id = sh->system_id;
      ctx.ship_id = det.ship_id;

      std::string msg;
      if (is_new) {
        msg = "New contact for " + fac->name + ": " + sh->name + " (" + other_name + ") in " + sys_name;
      } else {
        msg = "Contact reacquired for " + fac->name + ": " + sh->name + " (" + other_name + ") in " + sys_name;
      }

      push_event(EventLevel::Info, EventCategory::Intel, msg, ctx);
    }
  }

  for (Id fid : faction_ids) {
    auto* fac = find_ptr(state_.factions, fid);
    if (!fac) continue;

    auto& detected_today = detected_today_by_faction[fac->id];
    std::sort(detected_today.begin(), detected_today.end());
    detected_today.erase(std::unique(detected_today.begin(), detected_today.end()), detected_today.end());

    std::vector<Id> contact_ship_ids;
    contact_ship_ids.reserve(fac->ship_contacts.size());
    for (const auto& [sid, _] : fac->ship_contacts) contact_ship_ids.push_back(sid);
    std::sort(contact_ship_ids.begin(), contact_ship_ids.end());

    for (Id sid : contact_ship_ids) {
      const auto itc = fac->ship_contacts.find(sid);
      if (itc == fac->ship_contacts.end()) continue;
      const Contact& c = itc->second;

      if (c.last_seen_day != now - 1) continue;
      if (std::binary_search(detected_today.begin(), detected_today.end(), sid)) continue;

      const auto* sys = find_ptr(state_.systems, c.system_id);
      const std::string sys_name = sys ? sys->name : std::string("(unknown)");
      const auto* other_f = find_ptr(state_.factions, c.last_seen_faction_id);
      const std::string other_name = other_f ? other_f->name : std::string("(unknown)");

      EventContext ctx;
      ctx.faction_id = fac->id;
      ctx.faction_id2 = c.last_seen_faction_id;
      ctx.system_id = c.system_id;
      ctx.ship_id = c.ship_id;

      const std::string ship_name = c.last_seen_name.empty() ? ("Ship " + std::to_string(c.ship_id)) : c.last_seen_name;
      const std::string msg = "Contact lost for " + fac->name + ": " + ship_name + " (" + other_name + ") in " + sys_name;

      push_event(EventLevel::Info, EventCategory::Intel, msg, ctx);
    }
  }
}

void Simulation::tick_shields() {
  const auto ship_ids = sorted_keys(state_.ships);
  for (Id sid : ship_ids) {
    auto* sh = find_ptr(state_.ships, sid);
    if (!sh) continue;
    if (sh->hp <= 0.0) continue;

    const auto* d = find_design(sh->design_id);
    if (!d) {
      // If we can't resolve the design, keep shields at 0 to avoid NaNs.
      sh->shields = 0.0;
      continue;
    }

    const auto p = compute_power_allocation(*d, sh->power_policy);

    const double max_sh = std::max(0.0, d->max_shields);
    if (max_sh <= 1e-9) {
      sh->shields = 0.0;
      continue;
    }

    // If shields are offline (either due to insufficient power or because
    // the ship's power policy disables them), treat them as fully down.
    if (!p.shields_online) {
      sh->shields = 0.0;
      continue;
    }

    // Initialize shields for older saves / freshly spawned ships.
    if (sh->shields < 0.0) sh->shields = max_sh;

    const double regen = std::max(0.0, d->shield_regen_per_day);
    sh->shields = std::clamp(sh->shields + regen, 0.0, max_sh);
  }
}

void Simulation::tick_colonies() {
  // Precompute faction-wide economy modifiers once per tick for determinism
  // and to avoid repeated tech scanning in inner loops.
  std::unordered_map<Id, FactionEconomyMultipliers> fac_mult;
  fac_mult.reserve(state_.factions.size());
  for (Id fid : sorted_keys(state_.factions)) {
    fac_mult.emplace(fid, compute_faction_economy_multipliers(content_, state_.factions.at(fid)));
  }

  const FactionEconomyMultipliers default_mult;
  auto mult_for = [&](Id fid) -> const FactionEconomyMultipliers& {
    auto it = fac_mult.find(fid);
    if (it == fac_mult.end()) return default_mult;
    return it->second;
  };

  // Aggregate mining requests so that multiple colonies on the same body share
  // finite deposits fairly (proportional allocation) and deterministically.
  //
  // Structure: body_id -> mineral -> [(colony_id, requested_tons_per_day), ...]
  std::unordered_map<Id, std::unordered_map<std::string, std::vector<std::pair<Id, double>>>> mine_reqs;
  mine_reqs.reserve(state_.colonies.size());

  for (Id cid : sorted_keys(state_.colonies)) {
    auto& colony = state_.colonies.at(cid);

    const double mining_mult = std::max(0.0, mult_for(colony.faction_id).mining);

    // --- Installation-based production ---
    for (const auto& [inst_id, count] : colony.installations) {
      if (count <= 0) continue;
      const auto dit = content_.installations.find(inst_id);
      if (dit == content_.installations.end()) continue;

      const InstallationDef& def = dit->second;
      if (def.produces_per_day.empty()) continue;

      if (is_mining_installation(def)) {
        // Mining: convert produces_per_day into a request against body deposits.
        const Id body_id = colony.body_id;

        // If the body is missing (invalid save / hand-edited state), fall back to
        // the older "unlimited" behaviour to avoid silently losing resources.
        if (body_id == kInvalidId || state_.bodies.find(body_id) == state_.bodies.end()) {
          for (const auto& [mineral, per_day] : def.produces_per_day) {
            colony.minerals[mineral] += per_day * static_cast<double>(count) * mining_mult;
          }
          continue;
        }
        for (const auto& [mineral, per_day] : def.produces_per_day) {
          const double req = per_day * static_cast<double>(count) * mining_mult;
          if (req <= 1e-12) continue;
          mine_reqs[body_id][mineral].push_back({cid, req});
        }
      }
    }

    // --- Population growth/decline ---
    //
    // This intentionally does not generate events (would be too spammy). The UI can
    // display the updated value and players can tune the rate via SimConfig.
    if (std::fabs(cfg_.population_growth_rate_per_year) > 1e-12 && colony.population_millions > 0.0) {
      const double per_day = cfg_.population_growth_rate_per_year / 365.25;
      const double next = colony.population_millions * (1.0 + per_day);
      colony.population_millions = std::max(0.0, next);
    }
  }

  // --- Execute mining extraction against finite deposits ---
  if (!mine_reqs.empty()) {
    std::vector<Id> body_ids;
    body_ids.reserve(mine_reqs.size());
    for (const auto& [bid, _] : mine_reqs) body_ids.push_back(bid);
    std::sort(body_ids.begin(), body_ids.end());

    for (Id bid : body_ids) {
      Body* body = find_ptr(state_.bodies, bid);
      if (!body) continue;

      auto& per_mineral = mine_reqs.at(bid);
      std::vector<std::string> minerals;
      minerals.reserve(per_mineral.size());
      for (const auto& [m, _] : per_mineral) minerals.push_back(m);
      std::sort(minerals.begin(), minerals.end());

      for (const std::string& mineral : minerals) {
        auto it_list = per_mineral.find(mineral);
        if (it_list == per_mineral.end()) continue;
        auto& list = it_list->second;
        if (list.empty()) continue;

        // Total requested extraction for this mineral on this body.
        double total_req = 0.0;
        for (const auto& [_, req] : list) {
          if (req > 0.0) total_req += req;
        }
        if (total_req <= 1e-12) continue;

        // If the deposit key is missing, treat it as unlimited for back-compat.
        auto it_dep = body->mineral_deposits.find(mineral);
        if (it_dep == body->mineral_deposits.end()) {
          for (const auto& [colony_id, req] : list) {
            if (req <= 1e-12) continue;
            if (auto* c = find_ptr(state_.colonies, colony_id)) {
              c->minerals[mineral] += req;
            }
          }
          continue;
        }

        const double before_raw = it_dep->second;
        const double before = std::max(0.0, before_raw);
        if (before <= 1e-9) {
          it_dep->second = 0.0;
          continue;
        }

        if (before + 1e-9 >= total_req) {
          // Enough deposit to satisfy everyone fully.
          for (const auto& [colony_id, req] : list) {
            if (req <= 1e-12) continue;
            if (auto* c = find_ptr(state_.colonies, colony_id)) {
              c->minerals[mineral] += req;
            }
          }
          it_dep->second = std::max(0.0, before - total_req);
        } else {
          // Not enough deposit: allocate proportionally.
          const double ratio = before / total_req;
          for (const auto& [colony_id, req] : list) {
            if (req <= 1e-12) continue;
            if (auto* c = find_ptr(state_.colonies, colony_id)) {
              c->minerals[mineral] += req * ratio;
            }
          }
          it_dep->second = 0.0;
        }

        // Depletion warning (once, at the moment a deposit hits zero).
        if (before > 1e-9 && it_dep->second <= 1e-9) {
          Id best_cid = kInvalidId;
          Id best_fid = kInvalidId;
          for (const auto& [colony_id, req] : list) {
            if (req <= 1e-12) continue;
            if (best_cid == kInvalidId || colony_id < best_cid) {
              best_cid = colony_id;
              if (const Colony* c = find_ptr(state_.colonies, colony_id)) {
                best_fid = c->faction_id;
              }
            }
          }

          EventContext ctx;
          ctx.system_id = body->system_id;
          ctx.colony_id = best_cid;
          ctx.faction_id = best_fid;

          const std::string msg = "Mineral deposit depleted on " + body->name + ": " + mineral;
          push_event(EventLevel::Warn, EventCategory::Construction, msg, ctx);
        }
      }
    }

  }

  // --- Execute non-mining industry production/consumption ---
  //
  // This stage runs *after* mining extraction so that freshly mined minerals can
  // be consumed by industry in the same day.
  for (Id cid : sorted_keys(state_.colonies)) {
    Colony& colony = state_.colonies.at(cid);

    const double industry_mult = std::max(0.0, mult_for(colony.faction_id).industry);

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

      const auto it_def = content_.installations.find(inst_id);
      if (it_def == content_.installations.end()) continue;
      const InstallationDef& def = it_def->second;

      // Mining is handled above against finite deposits.
      if (is_mining_installation(def)) continue;

      if (def.produces_per_day.empty() && def.consumes_per_day.empty()) continue;

      // Compute the fraction of full-rate operation we can support with available inputs.
      double frac = 1.0;
      for (const auto& [mineral, per_day_raw] : def.consumes_per_day) {
        const double per_day = std::max(0.0, per_day_raw);
        if (per_day <= 1e-12) continue;

        const double req = per_day * static_cast<double>(count);

        const double have = [&]() {
          const auto it = colony.minerals.find(mineral);
          if (it == colony.minerals.end()) return 0.0;
          return std::max(0.0, it->second);
        }();

        if (req > 1e-12) frac = std::min(frac, have / req);
      }

      frac = std::clamp(frac, 0.0, 1.0);
      if (frac <= 1e-12) continue;

      // Consume inputs first (based on the computed fraction), then produce outputs.
      for (const auto& [mineral, per_day_raw] : def.consumes_per_day) {
        const double per_day = std::max(0.0, per_day_raw);
        if (per_day <= 1e-12) continue;
        const double amt = per_day * static_cast<double>(count) * frac;
        if (amt <= 1e-12) continue;

        double& stock = colony.minerals[mineral]; // creates entry if missing
        stock = std::max(0.0, stock - amt);
        if (stock <= 1e-9) stock = 0.0;
      }

      for (const auto& [mineral, per_day_raw] : def.produces_per_day) {
        const double per_day = std::max(0.0, per_day_raw);
        if (per_day <= 1e-12) continue;
        const double amt = per_day * static_cast<double>(count) * frac * industry_mult;
        if (amt <= 1e-12) continue;
        colony.minerals[mineral] += amt;
      }
    }
  }
}


void Simulation::tick_research() {
  std::unordered_map<Id, FactionEconomyMultipliers> fac_mult;
  fac_mult.reserve(state_.factions.size());
  for (Id fid : sorted_keys(state_.factions)) {
    fac_mult.emplace(fid, compute_faction_economy_multipliers(content_, state_.factions.at(fid)));
  }

  const FactionEconomyMultipliers default_mult;
  auto mult_for = [&](Id fid) -> const FactionEconomyMultipliers& {
    auto it = fac_mult.find(fid);
    if (it == fac_mult.end()) return default_mult;
    return it->second;
  };

  for (Id cid : sorted_keys(state_.colonies)) {
    auto& col = state_.colonies.at(cid);
    double rp_per_day = 0.0;
    for (const auto& [inst_id, count] : col.installations) {
      const auto dit = content_.installations.find(inst_id);
      if (dit == content_.installations.end()) continue;
      rp_per_day += dit->second.research_points_per_day * static_cast<double>(count);
    }
    if (rp_per_day <= 0.0) continue;
    rp_per_day *= std::max(0.0, mult_for(col.faction_id).research);
    if (rp_per_day <= 0.0) continue;
    auto fit = state_.factions.find(col.faction_id);
    if (fit == state_.factions.end()) continue;
    fit->second.research_points += rp_per_day;
  }

  auto prereqs_met = [&](const Faction& f, const TechDef& t) {
    for (const auto& p : t.prereqs) {
      if (!faction_has_tech(f, p)) return false;
    }
    return true;
  };

  for (Id fid : sorted_keys(state_.factions)) {
    auto& fac = state_.factions.at(fid);
    auto enqueue_unique = [&](const std::string& tech_id) {
      if (tech_id.empty()) return;
      if (faction_has_tech(fac, tech_id)) return;
      if (std::find(fac.research_queue.begin(), fac.research_queue.end(), tech_id) != fac.research_queue.end()) return;
      fac.research_queue.push_back(tech_id);
    };

    auto clean_queue = [&]() {
      auto keep = [&](const std::string& id) {
        if (id.empty()) return false;
        if (faction_has_tech(fac, id)) return false;
        return (content_.techs.find(id) != content_.techs.end());
      };
      fac.research_queue.erase(std::remove_if(fac.research_queue.begin(), fac.research_queue.end(),
                                             [&](const std::string& id) { return !keep(id); }),
                               fac.research_queue.end());
    };

    auto select_next_available = [&]() {
      clean_queue();
      fac.active_research_id.clear();
      fac.active_research_progress = 0.0;

      for (std::size_t i = 0; i < fac.research_queue.size(); ++i) {
        const std::string& id = fac.research_queue[i];
        const auto it = content_.techs.find(id);
        if (it == content_.techs.end()) continue;
        if (!prereqs_met(fac, it->second)) continue;

        fac.active_research_id = id;
        fac.active_research_progress = 0.0;
        fac.research_queue.erase(fac.research_queue.begin() + static_cast<std::ptrdiff_t>(i));
        return;
      }
    };

    if (!fac.active_research_id.empty()) {
      if (faction_has_tech(fac, fac.active_research_id)) {
        fac.active_research_id.clear();
        fac.active_research_progress = 0.0;
      } else {
        const auto it = content_.techs.find(fac.active_research_id);
        if (it == content_.techs.end()) {
          fac.active_research_id.clear();
          fac.active_research_progress = 0.0;
        } else if (!prereqs_met(fac, it->second)) {
          enqueue_unique(fac.active_research_id);
          fac.active_research_id.clear();
          fac.active_research_progress = 0.0;
        }
      }
    }

    if (fac.active_research_id.empty()) select_next_available();

    for (;;) {
      if (fac.active_research_id.empty()) break;
      const auto it2 = content_.techs.find(fac.active_research_id);
      if (it2 == content_.techs.end()) {
        fac.active_research_id.clear();
        fac.active_research_progress = 0.0;
        select_next_available();
        continue;
      }

      const TechDef& tech = it2->second;
      if (faction_has_tech(fac, tech.id)) {
        fac.active_research_id.clear();
        fac.active_research_progress = 0.0;
        select_next_available();
        continue;
      }

      if (!prereqs_met(fac, tech)) {
        enqueue_unique(tech.id);
        fac.active_research_id.clear();
        fac.active_research_progress = 0.0;
        select_next_available();
        continue;
      }

      const double remaining = std::max(0.0, tech.cost - fac.active_research_progress);

      if (remaining <= 0.0) {
        fac.known_techs.push_back(tech.id);
        for (const auto& eff : tech.effects) {
          if (eff.type == "unlock_component") {
            push_unique(fac.unlocked_components, eff.value);
          } else if (eff.type == "unlock_installation") {
            push_unique(fac.unlocked_installations, eff.value);
          }
        }
        {
          const std::string msg = "Research complete for " + fac.name + ": " + tech.name;
          log::info(msg);
          EventContext ctx;
          ctx.faction_id = fac.id;
          push_event(EventLevel::Info, EventCategory::Research, msg, ctx);
        }
        fac.active_research_id.clear();
        fac.active_research_progress = 0.0;
        select_next_available();
        continue;
      }

      if (fac.research_points <= 0.0) break;
      const double spend = std::min(fac.research_points, remaining);
      fac.research_points -= spend;
      fac.active_research_progress += spend;
    }
  }
}


void Simulation::tick_shipyards() {
  const auto it_def = content_.installations.find("shipyard");
  if (it_def == content_.installations.end()) return;

  std::unordered_map<Id, FactionEconomyMultipliers> fac_mult;
  fac_mult.reserve(state_.factions.size());
  for (Id fid : sorted_keys(state_.factions)) {
    fac_mult.emplace(fid, compute_faction_economy_multipliers(content_, state_.factions.at(fid)));
  }

  const FactionEconomyMultipliers default_mult;
  auto mult_for = [&](Id fid) -> const FactionEconomyMultipliers& {
    auto it = fac_mult.find(fid);
    if (it == fac_mult.end()) return default_mult;
    return it->second;
  };

  const InstallationDef& shipyard_def = it_def->second;
  const double base_rate = shipyard_def.build_rate_tons_per_day;
  if (base_rate <= 0.0) return;

  const auto& costs_per_ton = shipyard_def.build_costs_per_ton;

  auto max_build_by_minerals = [&](const Colony& colony, double desired_tons) {
    double max_tons = desired_tons;
    for (const auto& [mineral, cost_per_ton] : costs_per_ton) {
      if (cost_per_ton <= 0.0) continue;
      const auto it = colony.minerals.find(mineral);
      const double available = (it == colony.minerals.end()) ? 0.0 : it->second;
      max_tons = std::min(max_tons, available / cost_per_ton);
    }
    return max_tons;
  };

  auto consume_minerals = [&](Colony& colony, double built_tons) {
    for (const auto& [mineral, cost_per_ton] : costs_per_ton) {
      if (cost_per_ton <= 0.0) continue;
      const double cost = built_tons * cost_per_ton;
      colony.minerals[mineral] = std::max(0.0, colony.minerals[mineral] - cost);
    }
  };

  for (Id cid : sorted_keys(state_.colonies)) {
    auto& colony = state_.colonies.at(cid);
    const auto it_yard = colony.installations.find("shipyard");
    const int yards = (it_yard != colony.installations.end()) ? it_yard->second : 0;
    if (yards <= 0) continue;

    const double shipyard_mult = std::max(0.0, mult_for(colony.faction_id).shipyard);
    double capacity_tons = base_rate * static_cast<double>(yards) * shipyard_mult;

    while (capacity_tons > 1e-9 && !colony.shipyard_queue.empty()) {
      auto& bo = colony.shipyard_queue.front();
      const bool is_refit = bo.is_refit();

      const auto* body = find_ptr(state_.bodies, colony.body_id);

      // If this is a refit order, ensure the target ship exists and is docked.
      // Refit orders are front-of-queue and will stall the shipyard until the ship arrives.
      Ship* refit_ship = nullptr;
      if (is_refit) {
        if (!body) {
          const std::string msg = "Shipyard refit stalled (missing colony body): " + colony.name;
          nebula4x::log::error(msg);
          EventContext ctx;
          ctx.faction_id = colony.faction_id;
          ctx.colony_id = colony.id;
          push_event(EventLevel::Error, EventCategory::Shipyard, msg, ctx);
          colony.shipyard_queue.erase(colony.shipyard_queue.begin());
          continue;
        }

        refit_ship = find_ptr(state_.ships, bo.refit_ship_id);
        if (!refit_ship) {
          const std::string msg = "Shipyard refit target ship not found; dropping order at " + colony.name;
          nebula4x::log::warn(msg);
          EventContext ctx;
          ctx.faction_id = colony.faction_id;
          ctx.colony_id = colony.id;
          push_event(EventLevel::Warn, EventCategory::Shipyard, msg, ctx);
          colony.shipyard_queue.erase(colony.shipyard_queue.begin());
          continue;
        }

        if (refit_ship->faction_id != colony.faction_id) {
          const std::string msg = "Shipyard refit target ship faction mismatch; dropping order at " + colony.name;
          nebula4x::log::warn(msg);
          EventContext ctx;
          ctx.faction_id = colony.faction_id;
          ctx.colony_id = colony.id;
          ctx.ship_id = refit_ship->id;
          push_event(EventLevel::Warn, EventCategory::Shipyard, msg, ctx);
          colony.shipyard_queue.erase(colony.shipyard_queue.begin());
          continue;
        }

        if (!is_ship_docked_at_colony(refit_ship->id, colony.id)) {
          // Stall until the ship arrives. (No event spam.)
          break;
        }

        // Prototype drydock behavior: refitting ships are pinned to the colony body and cannot
        // execute other queued orders while their refit is being processed.
        refit_ship->position_mkm = body->position_mkm;
        state_.ship_orders[refit_ship->id].queue.clear();
        refit_ship->auto_explore = false;
        refit_ship->auto_freight = false;
      }

      double build_tons = std::min(capacity_tons, bo.tons_remaining);

      if (!costs_per_ton.empty()) {
        build_tons = max_build_by_minerals(colony, build_tons);
      }

      if (build_tons <= 1e-9) break;

      if (!costs_per_ton.empty()) consume_minerals(colony, build_tons);
      bo.tons_remaining -= build_tons;
      capacity_tons -= build_tons;

      if (bo.tons_remaining > 1e-9) break;

      // --- Completion ---
      if (is_refit) {
        const auto* target = find_design(bo.design_id);
        if (!target || !refit_ship) {
          const std::string msg = std::string("Shipyard refit failed (unknown design or missing ship): ") + bo.design_id;
          nebula4x::log::warn(msg);
          EventContext ctx;
          ctx.faction_id = colony.faction_id;
          ctx.colony_id = colony.id;
          if (refit_ship) ctx.ship_id = refit_ship->id;
          push_event(EventLevel::Warn, EventCategory::Shipyard, msg, ctx);
        } else {
          // Apply the new design. Treat a completed refit as a full overhaul (fully repaired).
          refit_ship->design_id = bo.design_id;
          refit_ship->hp = std::max(1.0, target->max_hp);
          apply_design_stats_to_ship(*refit_ship);

          if (body) refit_ship->position_mkm = body->position_mkm;

          // If the refit reduces cargo capacity, move excess cargo back into colony stockpiles.
          const double cap = std::max(0.0, target->cargo_tons);
          double used = 0.0;
          for (const auto& [_, tons] : refit_ship->cargo) used += std::max(0.0, tons);

          if (used > cap + 1e-9) {
            double excess = used - cap;
            for (const auto& mineral : sorted_keys(refit_ship->cargo)) {
              if (excess <= 1e-9) break;
              auto it = refit_ship->cargo.find(mineral);
              if (it == refit_ship->cargo.end()) continue;
              const double have = std::max(0.0, it->second);
              if (have <= 1e-9) continue;

              const double move = std::min(have, excess);
              it->second -= move;
              colony.minerals[mineral] += move;
              excess -= move;

              if (it->second <= 1e-9) refit_ship->cargo.erase(it);
            }
          }

          const std::string msg =
              "Refit ship " + refit_ship->name + " -> " + target->name + " (" + refit_ship->design_id + ") at " + colony.name;
          nebula4x::log::info(msg);
          EventContext ctx;
          ctx.faction_id = colony.faction_id;
          ctx.system_id = refit_ship->system_id;
          ctx.ship_id = refit_ship->id;
          ctx.colony_id = colony.id;
          push_event(EventLevel::Info, EventCategory::Shipyard, msg, ctx);
        }

        colony.shipyard_queue.erase(colony.shipyard_queue.begin());
        continue;
      }

      // Build new ship.
      const auto* design = find_design(bo.design_id);
      if (!design) {
        const std::string msg = std::string("Unknown design in build queue: ") + bo.design_id;
        nebula4x::log::warn(msg);
        EventContext ctx;
        ctx.faction_id = colony.faction_id;
        ctx.colony_id = colony.id;
        push_event(EventLevel::Warn, EventCategory::Shipyard, msg, ctx);
      } else if (!body) {
        const std::string msg = "Shipyard build failed (missing colony body): " + colony.name;
        nebula4x::log::error(msg);
        EventContext ctx;
        ctx.faction_id = colony.faction_id;
        ctx.colony_id = colony.id;
        push_event(EventLevel::Error, EventCategory::Shipyard, msg, ctx);
      } else if (auto* sys = find_ptr(state_.systems, body->system_id); !sys) {
        const std::string msg = "Shipyard build failed (missing system): colony=" + colony.name;
        nebula4x::log::error(msg);
        EventContext ctx;
        ctx.faction_id = colony.faction_id;
        ctx.colony_id = colony.id;
        push_event(EventLevel::Error, EventCategory::Shipyard, msg, ctx);
      } else {
        Ship sh;
        sh.id = allocate_id(state_);
        sh.faction_id = colony.faction_id;
        sh.system_id = body->system_id;
        sh.design_id = bo.design_id;
        sh.position_mkm = body->position_mkm;
        sh.fuel_tons = 0.0;
        apply_design_stats_to_ship(sh);
        sh.name = design->name + " #" + std::to_string(sh.id);
        state_.ships[sh.id] = sh;
        state_.ship_orders[sh.id] = ShipOrders{};
        state_.systems[sh.system_id].ships.push_back(sh.id);

        const std::string msg = "Built ship " + sh.name + " (" + sh.design_id + ") at " + colony.name;
        nebula4x::log::info(msg);
        EventContext ctx;
        ctx.faction_id = colony.faction_id;
        ctx.system_id = sh.system_id;
        ctx.ship_id = sh.id;
        ctx.colony_id = colony.id;
        push_event(EventLevel::Info, EventCategory::Shipyard, msg, ctx);
      }

      colony.shipyard_queue.erase(colony.shipyard_queue.begin());
    }
  }
}

void Simulation::tick_construction() {
  for (Id cid : sorted_keys(state_.colonies)) {
    auto& colony = state_.colonies.at(cid);

    Id colony_system_id = kInvalidId;
    if (auto* b = find_ptr(state_.bodies, colony.body_id)) colony_system_id = b->system_id;

    double cp_available = construction_points_per_day(colony);
    if (cp_available <= 1e-9) continue;

    auto can_pay_minerals = [&](const InstallationDef& def) {
      for (const auto& [mineral, cost] : def.build_costs) {
        if (cost <= 0.0) continue;
        const auto it = colony.minerals.find(mineral);
        const double have = (it == colony.minerals.end()) ? 0.0 : it->second;
        if (have + 1e-9 < cost) return false;
      }
      return true;
    };

    auto pay_minerals = [&](const InstallationDef& def) {
      for (const auto& [mineral, cost] : def.build_costs) {
        if (cost <= 0.0) continue;
        colony.minerals[mineral] = std::max(0.0, colony.minerals[mineral] - cost);
      }
    };

    // Construction queue processing:
    //
    // Previous behavior was strictly "front-of-queue only" which meant a single
    // unaffordable order (missing minerals) could block the entire queue forever.
    //
    // New behavior:
    // - The sim will *skip* stalled orders (can't pay minerals) and continue trying
    //   later orders in the same day. This prevents total queue lock-ups.
    // - If construction points remain, the sim may also apply CP to multiple queued
    //   orders in a single day (a simple form of parallelization).
    //
    // This keeps the model simple while making colony production far less brittle.
    auto& q = colony.construction_queue;

    int safety_steps = 0;
    constexpr int kMaxSteps = 100000;

    while (cp_available > 1e-9 && !q.empty() && safety_steps++ < kMaxSteps) {
      bool progressed_any = false;

      for (std::size_t i = 0; i < q.size() && cp_available > 1e-9;) {
        auto& ord = q[i];

        if (ord.quantity_remaining <= 0) {
          q.erase(q.begin() + static_cast<std::ptrdiff_t>(i));
          progressed_any = true;
          continue;
        }

        auto it_def = content_.installations.find(ord.installation_id);
        if (it_def == content_.installations.end()) {
          q.erase(q.begin() + static_cast<std::ptrdiff_t>(i));
          progressed_any = true;
          continue;
        }
        const InstallationDef& def = it_def->second;

        auto complete_one = [&]() {
          colony.installations[def.id] += 1;
          ord.quantity_remaining -= 1;
          ord.minerals_paid = false;
          ord.cp_remaining = 0.0;

          const std::string msg = "Constructed " + def.name + " at " + colony.name;
          EventContext ctx;
          ctx.faction_id = colony.faction_id;
          ctx.system_id = colony_system_id;
          ctx.colony_id = colony.id;
          push_event(EventLevel::Info, EventCategory::Construction, msg, ctx);

          progressed_any = true;

          if (ord.quantity_remaining <= 0) {
            q.erase(q.begin() + static_cast<std::ptrdiff_t>(i));
            return;
          }

          // Keep i the same so we can immediately attempt the next unit of this
          // same order in the same day (if we still have CP and minerals).
        };

        // If we haven't started the current unit, attempt to pay minerals.
        if (!ord.minerals_paid) {
          if (!can_pay_minerals(def)) {
            // Stalled: skip this order for now (do not block the whole queue).
            ++i;
            continue;
          }

          pay_minerals(def);
          ord.minerals_paid = true;
          ord.cp_remaining = std::max(0.0, def.construction_cost);
          progressed_any = true;

          if (ord.cp_remaining <= 1e-9) {
            // Instant build (0 CP cost).
            complete_one();
            continue;
          }
        } else {
          // Defensive repair: if an in-progress unit was loaded with cp_remaining == 0
          // but the definition has a CP cost, restore the remaining CP from the def.
          if (ord.cp_remaining <= 1e-9 && def.construction_cost > 0.0) {
            ord.cp_remaining = def.construction_cost;
          }
        }

        // Spend CP on the in-progress unit.
        if (ord.minerals_paid && ord.cp_remaining > 1e-9) {
          const double spend = std::min(cp_available, ord.cp_remaining);
          ord.cp_remaining -= spend;
          cp_available -= spend;
          progressed_any = true;

          if (ord.cp_remaining <= 1e-9) {
            complete_one();
            continue;
          }
        }

        ++i;
      }

      // If we made no progress in an entire scan of the queue, stop to avoid an
      // infinite loop (e.g. all remaining orders are stalled on minerals).
      if (!progressed_any) break;
    }
  }
}


void Simulation::tick_ai() {
  // Economic planning for AI factions (research, construction, shipbuilding).
  tick_ai_economy(*this);
  const auto ship_ids = sorted_keys(state_.ships);
  const auto faction_ids = sorted_keys(state_.factions);

  auto orders_empty = [&](Id ship_id) -> bool {
    auto it = state_.ship_orders.find(ship_id);
    if (it == state_.ship_orders.end()) return true;
    const ShipOrders& so = it->second;
    if (!so.queue.empty()) return false;
    // A ship with repeat enabled and remaining refills is not considered idle:
    // its queue will be refilled during tick_ships().
    if (so.repeat && !so.repeat_template.empty() && so.repeat_count_remaining != 0) return false;
    return true;
  };

  auto role_priority = [&](ShipRole r) -> int {
    // Pirates like easy prey first.
    switch (r) {
      case ShipRole::Freighter: return 0;
      case ShipRole::Surveyor: return 1;
      case ShipRole::Combatant: return 2;
      default: return 3;
    }
  };

  auto issue_auto_explore = [&](Id ship_id) -> bool {
    Ship* ship = find_ptr(state_.ships, ship_id);
    if (!ship) return false;
    if (!orders_empty(ship_id)) return false;
    if (ship->system_id == kInvalidId) return false;
    if (ship->speed_km_s <= 0.0) return false;

    // Avoid fighting the fleet movement logic. Fleets should be controlled by fleet orders.
    if (fleet_for_ship(ship_id) != kInvalidId) return false;

    const Id fid = ship->faction_id;
    const auto* sys = find_ptr(state_.systems, ship->system_id);
    if (!sys) return false;

    std::vector<Id> jps = sys->jump_points;
    std::sort(jps.begin(), jps.end());

    // If we have an undiscovered neighbor, jump now.
    for (Id jp_id : jps) {
      const JumpPoint* jp = find_ptr(state_.jump_points, jp_id);
      if (!jp) continue;
      const JumpPoint* other = find_ptr(state_.jump_points, jp->linked_jump_id);
      if (!other) continue;
      const Id dest_sys = other->system_id;
      if (dest_sys == kInvalidId) continue;
      if (!is_system_discovered_by_faction(fid, dest_sys)) {
        issue_travel_via_jump(ship_id, jp_id);
        return true;
      }
    }

    // Otherwise, route to the nearest *discovered* frontier system (one jump away from an undiscovered neighbor).
    std::unordered_set<Id> visited;
    std::queue<Id> q;
    visited.insert(ship->system_id);
    q.push(ship->system_id);

    Id frontier = kInvalidId;

    while (!q.empty()) {
      const Id cur = q.front();
      q.pop();

      const auto* cs = find_ptr(state_.systems, cur);
      if (!cs) continue;

      std::vector<Id> cs_jps = cs->jump_points;
      std::sort(cs_jps.begin(), cs_jps.end());

      bool is_frontier = false;
      for (Id jp_id : cs_jps) {
        const JumpPoint* jp = find_ptr(state_.jump_points, jp_id);
        if (!jp) continue;
        const JumpPoint* other = find_ptr(state_.jump_points, jp->linked_jump_id);
        if (!other) continue;
        const Id dest_sys = other->system_id;
        if (dest_sys == kInvalidId) continue;
        if (!is_system_discovered_by_faction(fid, dest_sys)) {
          is_frontier = true;
          break;
        }
      }

      if (is_frontier) {
        frontier = cur;
        break;
      }

      for (Id jp_id : cs_jps) {
        const JumpPoint* jp = find_ptr(state_.jump_points, jp_id);
        if (!jp) continue;
        const JumpPoint* other = find_ptr(state_.jump_points, jp->linked_jump_id);
        if (!other) continue;
        const Id dest_sys = other->system_id;
        if (dest_sys == kInvalidId) continue;
        if (!is_system_discovered_by_faction(fid, dest_sys)) continue;
        if (visited.insert(dest_sys).second) q.push(dest_sys);
      }
    }

    if (frontier != kInvalidId && frontier != ship->system_id) {
      return issue_travel_to_system(ship_id, frontier, true);
    }

    return false;
  };

  // --- Ship-level automation: Auto-explore ---
  for (Id sid : ship_ids) {
    Ship* sh = find_ptr(state_.ships, sid);
    if (!sh) continue;
    if (!sh->auto_explore) continue;
    if (sh->auto_freight) continue;  // mutually exclusive; auto-freight handled below
    if (!orders_empty(sid)) continue;

    (void)issue_auto_explore(sid);
  }


  // --- Ship-level automation: Auto-freight (mineral logistics) ---
  auto cargo_used_tons = [](const Ship& s) {
    double used = 0.0;
    for (const auto& [_, tons] : s.cargo) used += std::max(0.0, tons);
    return used;
  };

  // Group idle auto-freight ships by faction so we can avoid over-assigning the same minerals.
  std::unordered_map<Id, std::vector<Id>> freight_ships_by_faction;
  freight_ships_by_faction.reserve(faction_ids.size() * 2);

  for (Id sid : ship_ids) {
    Ship* sh = find_ptr(state_.ships, sid);
    if (!sh) continue;
    if (!sh->auto_freight) continue;
    if (sh->auto_explore) continue;  // mutually exclusive; auto-explore handled above
    if (!orders_empty(sid)) continue;
    if (sh->system_id == kInvalidId) continue;
    if (sh->speed_km_s <= 0.0) continue;

    // Avoid fighting the fleet movement logic. Fleets should be controlled by fleet orders.
    if (fleet_for_ship(sid) != kInvalidId) continue;

    const auto* d = find_design(sh->design_id);
    const double cap = d ? std::max(0.0, d->cargo_tons) : 0.0;
    if (cap <= 1e-9) continue;

    freight_ships_by_faction[sh->faction_id].push_back(sid);
  }

  auto estimate_eta_days_to_pos = [&](Id start_system_id, Vec2 start_pos_mkm, Id fid, double speed_km_s,
                                     Id goal_system_id, Vec2 goal_pos_mkm) -> double {
    if (speed_km_s <= 0.0) return std::numeric_limits<double>::infinity();
    const double mkm_per_day = mkm_per_day_from_speed(speed_km_s, cfg_.seconds_per_day);
    if (mkm_per_day <= 0.0) return std::numeric_limits<double>::infinity();

    if (start_system_id == goal_system_id) {
      return (goal_pos_mkm - start_pos_mkm).length() / mkm_per_day;
    }

    auto plan = plan_jump_route_cached(start_system_id, start_pos_mkm, fid, speed_km_s, goal_system_id,
                                      /*restrict_to_discovered=*/true);
    if (!plan) return std::numeric_limits<double>::infinity();

    double eta = plan->eta_days;

    // Arrival position in the goal system is the entry jump point of the last hop.
    Vec2 arrival = start_pos_mkm;
    if (!plan->jump_ids.empty()) {
      const Id last_exit = plan->jump_ids.back();
      const JumpPoint* exit_jp = find_ptr(state_.jump_points, last_exit);
      if (exit_jp && exit_jp->linked_jump_id != kInvalidId) {
        if (const JumpPoint* entry_jp = find_ptr(state_.jump_points, exit_jp->linked_jump_id)) {
          arrival = entry_jp->position_mkm;
        }
      }
    }

    eta += (goal_pos_mkm - arrival).length() / mkm_per_day;
    return eta;
  };

  for (Id fid : faction_ids) {
    auto it_auto = freight_ships_by_faction.find(fid);
    if (it_auto == freight_ships_by_faction.end()) continue;

    // Gather colonies for this faction and their body positions.
    std::vector<Id> colony_ids;
    colony_ids.reserve(state_.colonies.size());
    std::unordered_map<Id, Id> colony_system;
    std::unordered_map<Id, Vec2> colony_pos;
    for (Id cid : sorted_keys(state_.colonies)) {
      const Colony* c = find_ptr(state_.colonies, cid);
      if (!c) continue;
      if (c->faction_id != fid) continue;
      const Body* b = find_ptr(state_.bodies, c->body_id);
      if (!b) continue;
      if (b->system_id == kInvalidId) continue;
      colony_ids.push_back(cid);
      colony_system[cid] = b->system_id;
      colony_pos[cid] = b->position_mkm;
    }

    if (colony_ids.empty()) continue;

    // Compute per-colony mineral reserves (to avoid starving the source colony's own queues),
    // and compute mineral shortfalls that we want to relieve.
    std::unordered_map<Id, std::unordered_map<std::string, double>> reserve_by_colony;
    std::unordered_map<Id, std::unordered_map<std::string, double>> missing_by_colony;
    const auto needs = logistics_needs_for_faction(fid);

    // Seed reserves from user-configured colony reserve settings.
    for (Id cid : colony_ids) {
      const Colony* c = find_ptr(state_.colonies, cid);
      if (!c) continue;
      for (const auto& [mineral, tons_raw] : c->mineral_reserves) {
        const double tons = std::max(0.0, tons_raw);
        if (tons <= 1e-9) continue;
        double& r = reserve_by_colony[cid][mineral];
        r = std::max(r, tons);
      }
    }


    for (const auto& n : needs) {
      // Reserve: keep enough at the colony to satisfy the local target (one day shipyard throughput or one build unit).
      double& r = reserve_by_colony[n.colony_id][n.mineral];
      r = std::max(r, std::max(0.0, n.desired_tons));

      const double missing = std::max(0.0, n.missing_tons);
      if (missing > 1e-9) {
        double& m = missing_by_colony[n.colony_id][n.mineral];
        m = std::max(m, missing);
      }
    }

    // Precompute per-destination mineral priority lists (descending missing tons).
    // This provides deterministic iteration order even though our storage is hash-based.
    std::unordered_map<Id, std::vector<std::string>> need_minerals_by_colony;
    need_minerals_by_colony.reserve(missing_by_colony.size() * 2 + 8);
    for (Id cid : colony_ids) {
      auto it_miss = missing_by_colony.find(cid);
      if (it_miss == missing_by_colony.end()) continue;

      std::vector<std::pair<std::string, double>> pairs;
      pairs.reserve(it_miss->second.size());
      for (const auto& [mineral, miss_raw] : it_miss->second) {
        const double miss = std::max(0.0, miss_raw);
        if (miss <= 1e-9) continue;
        pairs.emplace_back(mineral, miss);
      }
      if (pairs.empty()) continue;

      std::sort(pairs.begin(), pairs.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
      });

      std::vector<std::string> minerals;
      minerals.reserve(pairs.size());
      for (const auto& [m, _] : pairs) minerals.push_back(m);
      need_minerals_by_colony[cid] = std::move(minerals);
    }

    // Stable lists of destinations and sources.
    std::vector<Id> dests_with_needs;
    dests_with_needs.reserve(need_minerals_by_colony.size());
    for (Id cid : colony_ids) {
      if (need_minerals_by_colony.find(cid) != need_minerals_by_colony.end()) dests_with_needs.push_back(cid);
    }

    // Compute exportable minerals for each colony = stockpile - local reserve.
    std::unordered_map<Id, std::unordered_map<std::string, double>> exportable_by_colony;
    exportable_by_colony.reserve(colony_ids.size() * 2);
    for (Id cid : colony_ids) {
      const Colony* c = find_ptr(state_.colonies, cid);
      if (!c) continue;
      for (const auto& [mineral, have_raw] : c->minerals) {
        const double have = std::max(0.0, have_raw);
        double reserve = 0.0;
        if (auto it_r = reserve_by_colony.find(cid); it_r != reserve_by_colony.end()) {
          if (auto it_m = it_r->second.find(mineral); it_m != it_r->second.end()) reserve = std::max(0.0, it_m->second);
        }
        const double surplus = std::max(0.0, have - reserve);
        if (surplus > 1e-9) {
          exportable_by_colony[cid][mineral] = surplus;
        }
      }
    }

    auto auto_ships = it_auto->second;
    std::sort(auto_ships.begin(), auto_ships.end());

    const bool bundle_multi = cfg_.auto_freight_multi_mineral;
    // Avoid degenerate "0 ton" shipments if the config is set to 0.
    const double min_tons = std::max(1e-6, cfg_.auto_freight_min_transfer_tons);
    const double take_frac = std::clamp(cfg_.auto_freight_max_take_fraction_of_surplus, 0.0, 1.0);

    struct FreightItem {
      std::string mineral;
      double tons{0.0};
    };

    auto dec_map_value = [](std::unordered_map<std::string, double>& m, const std::string& key, double amount) {
      if (amount <= 0.0) return;
      auto it = m.find(key);
      if (it == m.end()) return;
      it->second = std::max(0.0, it->second - amount);
      if (it->second <= 1e-9) m.erase(it);
    };

    auto dec_missing = [&](Id cid, const std::string& mineral, double amount) {
      if (amount <= 0.0) return;
      auto itc = missing_by_colony.find(cid);
      if (itc == missing_by_colony.end()) return;
      dec_map_value(itc->second, mineral, amount);
      if (itc->second.empty()) missing_by_colony.erase(itc);
    };

    auto dec_exportable = [&](Id cid, const std::string& mineral, double amount) {
      if (amount <= 0.0) return;
      auto itc = exportable_by_colony.find(cid);
      if (itc == exportable_by_colony.end()) return;
      dec_map_value(itc->second, mineral, amount);
      if (itc->second.empty()) exportable_by_colony.erase(itc);
    };

    for (Id sid : auto_ships) {
      Ship* sh = find_ptr(state_.ships, sid);
      if (!sh) continue;
      if (!orders_empty(sid)) continue;
      if (sh->system_id == kInvalidId) continue;

      const auto* d = find_design(sh->design_id);
      const double cap = d ? std::max(0.0, d->cargo_tons) : 0.0;
      if (cap <= 1e-9) continue;

      const double used = cargo_used_tons(*sh);
      const double free = std::max(0.0, cap - used);

      // 1) If we already have cargo, try to deliver it (optionally bundling multiple minerals)
      //    to a single colony that needs them.
      bool assigned = false;
      if (used > 1e-9 && !dests_with_needs.empty()) {
        std::vector<std::string> cargo_minerals;
        cargo_minerals.reserve(sh->cargo.size());
        for (const auto& [m, tons_raw] : sh->cargo) {
          if (std::max(0.0, tons_raw) > 1e-9) cargo_minerals.push_back(m);
        }
        std::sort(cargo_minerals.begin(), cargo_minerals.end());

        struct UnloadChoice {
          Id dest{kInvalidId};
          double eff{std::numeric_limits<double>::infinity()};
          double eta{std::numeric_limits<double>::infinity()};
          double total{0.0};
          std::vector<FreightItem> items;
        } best;

        for (Id dest_cid : dests_with_needs) {
          if (dest_cid == kInvalidId) continue;
          auto it_sys = colony_system.find(dest_cid);
          auto it_pos = colony_pos.find(dest_cid);
          if (it_sys == colony_system.end() || it_pos == colony_pos.end()) continue;

          std::vector<FreightItem> items;
          items.reserve(bundle_multi ? cargo_minerals.size() : 1);
          double total = 0.0;

          for (const auto& mineral : cargo_minerals) {
            const double have = [&]() {
              auto it = sh->cargo.find(mineral);
              return (it == sh->cargo.end()) ? 0.0 : std::max(0.0, it->second);
            }();
            if (have < min_tons) continue;

            const double miss = [&]() {
              auto itc = missing_by_colony.find(dest_cid);
              if (itc == missing_by_colony.end()) return 0.0;
              auto itm = itc->second.find(mineral);
              if (itm == itc->second.end()) return 0.0;
              return std::max(0.0, itm->second);
            }();
            if (miss < min_tons) continue;

            const double amount = std::min(have, miss);
            if (amount < min_tons) continue;

            items.push_back(FreightItem{mineral, amount});
            total += amount;

            if (!bundle_multi) break;
          }

          if (total < min_tons) continue;
          const double eta = estimate_eta_days_to_pos(sh->system_id, sh->position_mkm, fid, sh->speed_km_s,
                                                     it_sys->second, it_pos->second);
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
          bool ok = true;
          for (const auto& it : best.items) {
            ok = ok && issue_unload_mineral(sid, best.dest, it.mineral, it.tons, /*restrict_to_discovered=*/true);
          }
          if (!ok) {
            (void)clear_orders(sid);
          } else {
            for (const auto& it : best.items) {
              dec_missing(best.dest, it.mineral, it.tons);
            }
            assigned = true;
          }
        }
      }

      if (assigned) continue;

      // 2) Otherwise, pick a source colony and destination colony, optionally bundling multiple minerals
      //    that the destination needs in a single trip.
      if (free < min_tons) continue;
      if (dests_with_needs.empty()) continue;
      if (exportable_by_colony.empty()) continue;

      // Candidate source colonies (sorted).
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
        std::vector<FreightItem> items;
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

          std::vector<FreightItem> items;
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

            items.push_back(FreightItem{mineral, amount});
            total += amount;
            remaining -= amount;

            if (!bundle_multi) break;
          }

          if (total < min_tons) continue;

          const double eta1 = estimate_eta_days_to_pos(sh->system_id, sh->position_mkm, fid, sh->speed_km_s,
                                                       it_src_sys->second, it_src_pos->second);
          if (!std::isfinite(eta1)) continue;
          const double eta2 = estimate_eta_days_to_pos(it_src_sys->second, it_src_pos->second, fid, sh->speed_km_s,
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
            best.items = std::move(items);
          }
        }
      }

      if (best.source != kInvalidId && best.dest != kInvalidId && !best.items.empty()) {
        bool ok = true;
        for (const auto& it : best.items) {
          ok = ok && issue_load_mineral(sid, best.source, it.mineral, it.tons, /*restrict_to_discovered=*/true);
        }
        for (const auto& it : best.items) {
          ok = ok && issue_unload_mineral(sid, best.dest, it.mineral, it.tons, /*restrict_to_discovered=*/true);
        }

        if (!ok) {
          (void)clear_orders(sid);
        } else {
          for (const auto& it : best.items) {
            dec_exportable(best.source, it.mineral, it.tons);
            dec_missing(best.dest, it.mineral, it.tons);
          }
        }
      }
    }
  }

  // --- Faction-level AI profiles ---
  const int now = static_cast<int>(state_.date.days_since_epoch());
  constexpr int kMaxChaseAgeDays = 60;

  for (Id fid : faction_ids) {
    Faction& fac = state_.factions.at(fid);

    if (fac.control == FactionControl::Player) continue;
    if (fac.control == FactionControl::AI_Passive) continue;

    if (fac.control == FactionControl::AI_Explorer) {
      for (Id sid : ship_ids) {
        Ship* sh = find_ptr(state_.ships, sid);
        if (!sh) continue;
        if (sh->faction_id != fid) continue;
        if (!orders_empty(sid)) continue;
        if (sh->auto_explore) continue;  // already handled above
        const auto* d = find_design(sh->design_id);
        if (d && d->role != ShipRole::Surveyor) continue;
        (void)issue_auto_explore(sid);
      }
      continue;
    }

    if (fac.control == FactionControl::AI_Pirate) {
      for (Id sid : ship_ids) {
        Ship* sh = find_ptr(state_.ships, sid);
        if (!sh) continue;
        if (sh->faction_id != fid) continue;
        if (!orders_empty(sid)) continue;
        if (sh->auto_explore) continue;  // allow manual override

        // 1) If hostiles are currently detected in-system, attack the best target.
        const auto hostiles = detected_hostile_ships_in_system(fid, sh->system_id);
        if (!hostiles.empty()) {
          Id best = kInvalidId;
          int best_prio = 999;
          double best_dist = 0.0;

          for (Id tid : hostiles) {
            const Ship* tgt = find_ptr(state_.ships, tid);
            if (!tgt) continue;
            const auto* td = find_design(tgt->design_id);
            const ShipRole tr = td ? td->role : ShipRole::Unknown;
            const int prio = role_priority(tr);
            const double dist = (tgt->position_mkm - sh->position_mkm).length();

            if (best == kInvalidId || prio < best_prio ||
                (prio == best_prio && (dist < best_dist - 1e-9 ||
                                       (std::abs(dist - best_dist) <= 1e-9 && tid < best)))) {
              best = tid;
              best_prio = prio;
              best_dist = dist;
            }
          }

          if (best != kInvalidId) {
            (void)issue_attack_ship(sid, best, true);
            continue;
          }
        }

        // 2) Otherwise, chase a recent hostile contact (last known intel).
        Id contact_target = kInvalidId;
        int best_day = -1;
        int best_prio = 999;

        for (const auto& [_, c] : fac.ship_contacts) {
          if (c.ship_id == kInvalidId) continue;
          if (c.last_seen_faction_id == fid) continue;  // friendly
          if (state_.ships.find(c.ship_id) == state_.ships.end()) continue;
          const int age = now - c.last_seen_day;
          if (age > kMaxChaseAgeDays) continue;
          if (!is_system_discovered_by_faction(fid, c.system_id)) continue;

          const auto* td = find_design(c.last_seen_design_id);
          const ShipRole tr = td ? td->role : ShipRole::Unknown;
          const int prio = role_priority(tr);

          if (c.last_seen_day > best_day || (c.last_seen_day == best_day && prio < best_prio) ||
              (c.last_seen_day == best_day && prio == best_prio && c.ship_id < contact_target)) {
            contact_target = c.ship_id;
            best_day = c.last_seen_day;
            best_prio = prio;
          }
        }

        if (contact_target != kInvalidId) {
          (void)issue_attack_ship(sid, contact_target, true);
          continue;
        }

        // 3) Roam: pick a jump point (prefer exploring undiscovered neighbors).
        const auto* sys = find_ptr(state_.systems, sh->system_id);
        if (!sys) continue;

        std::vector<Id> jps = sys->jump_points;
        std::sort(jps.begin(), jps.end());

        Id chosen = kInvalidId;
        Id fallback = kInvalidId;
        for (Id jp_id : jps) {
          const JumpPoint* jp = find_ptr(state_.jump_points, jp_id);
          if (!jp) continue;
          const JumpPoint* other = find_ptr(state_.jump_points, jp->linked_jump_id);
          if (!other) continue;
          const Id dest_sys = other->system_id;
          if (dest_sys == kInvalidId) continue;

          if (fallback == kInvalidId) fallback = jp_id;
          if (!is_system_discovered_by_faction(fid, dest_sys)) {
            chosen = jp_id;
            break;
          }
        }
        if (chosen == kInvalidId) chosen = fallback;

        if (chosen != kInvalidId) {
          (void)issue_travel_via_jump(sid, chosen);
        }
      }
      continue;
    }
  }
}

void Simulation::tick_refuel() {
  constexpr const char* kFuelKey = "Fuel";

  // Fast(ish) lookup: system -> colony ids.
  std::unordered_map<Id, std::vector<Id>> colonies_in_system;
  colonies_in_system.reserve(state_.systems.size() * 2 + 8);

  for (const auto& [cid, col] : state_.colonies) {
    const auto* body = find_ptr(state_.bodies, col.body_id);
    if (!body) continue;
    colonies_in_system[body->system_id].push_back(cid);
  }

  const double arrive_eps = std::max(0.0, cfg_.arrival_epsilon_mkm);
  const double dock_range = std::max(arrive_eps, cfg_.docking_range_mkm);

  for (auto& [sid, ship] : state_.ships) {
    const ShipDesign* d = find_design(ship.design_id);
    if (!d) continue;

    const double cap = std::max(0.0, d->fuel_capacity_tons);
    if (cap <= 1e-9) continue;

    // Clamp away any weird negative sentinel states before using.
    ship.fuel_tons = std::clamp(ship.fuel_tons, 0.0, cap);

    const double need = cap - ship.fuel_tons;
    if (need <= 1e-9) continue;

    auto it = colonies_in_system.find(ship.system_id);
    if (it == colonies_in_system.end()) continue;

    Id best_cid = kInvalidId;
    double best_dist = 1e100;

    for (Id cid : it->second) {
      const Colony* col = find_ptr(state_.colonies, cid);
      if (!col) continue;
      if (col->faction_id != ship.faction_id) continue;

      const Body* body = find_ptr(state_.bodies, col->body_id);
      if (!body) continue;
      const double dist = (body->position_mkm - ship.position_mkm).length();
      if (dist > dock_range + 1e-9) continue;

      if (dist < best_dist) {
        best_dist = dist;
        best_cid = cid;
      }
    }

    if (best_cid == kInvalidId) continue;

    Colony& col = state_.colonies.at(best_cid);
    const double avail = col.minerals[kFuelKey];
    if (avail <= 1e-9) continue;

    const double take = std::min(need, avail);
    ship.fuel_tons += take;
    col.minerals[kFuelKey] = avail - take;
    if (col.minerals[kFuelKey] <= 1e-9) col.minerals[kFuelKey] = 0.0;
  }
}


void Simulation::tick_repairs() {
  const double per_yard = std::max(0.0, cfg_.repair_hp_per_day_per_shipyard);
  if (per_yard <= 0.0) return;

  const double dock_range = std::max(0.0, cfg_.docking_range_mkm);

  const auto ship_ids = sorted_keys(state_.ships);
  for (Id sid : ship_ids) {
    auto* ship = find_ptr(state_.ships, sid);
    if (!ship) continue;

    const auto* d = find_design(ship->design_id);
    const double max_hp = d ? d->max_hp : ship->hp;
    if (max_hp <= 0.0) continue;

    // Clamp just in case something drifted out of bounds (custom content, etc.).
    if (ship->hp > max_hp) ship->hp = max_hp;
    if (ship->hp >= max_hp - 1e-9) continue;

    Id best_colony = kInvalidId;
    int best_shipyards = 0;
    double best_dist = 0.0;

    const auto colony_ids = sorted_keys(state_.colonies);
    for (Id cid : colony_ids) {
      const auto* colony = find_ptr(state_.colonies, cid);
      if (!colony) continue;
      if (colony->faction_id != ship->faction_id) continue;

      const auto it_yard = colony->installations.find("shipyard");
      const int yards = (it_yard != colony->installations.end()) ? it_yard->second : 0;
      if (yards <= 0) continue;

      const auto* body = find_ptr(state_.bodies, colony->body_id);
      if (!body) continue;
      if (body->system_id != ship->system_id) continue;

      const double dist = (ship->position_mkm - body->position_mkm).length();
      if (dist > dock_range + 1e-9) continue;

      // Prefer the colony with the most shipyards, then the closest distance, then lowest id.
      bool better = false;
      if (yards > best_shipyards) {
        better = true;
      } else if (yards == best_shipyards) {
        if (best_colony == kInvalidId || dist < best_dist - 1e-9) {
          better = true;
        } else if (std::abs(dist - best_dist) <= 1e-9 && cid < best_colony) {
          better = true;
        }
      }

      if (better) {
        best_colony = cid;
        best_shipyards = yards;
        best_dist = dist;
      }
    }

    if (best_colony == kInvalidId || best_shipyards <= 0) continue;

    const double before = ship->hp;
    ship->hp = std::min(max_hp, ship->hp + per_yard * static_cast<double>(best_shipyards));

    if (before < max_hp - 1e-9 && ship->hp >= max_hp - 1e-9) {
      // Log only when the ship is fully repaired to avoid event spam.
      const auto* colony = find_ptr(state_.colonies, best_colony);
      const auto* sys = find_ptr(state_.systems, ship->system_id);

      EventContext ctx;
      ctx.faction_id = ship->faction_id;
      ctx.system_id = ship->system_id;
      ctx.ship_id = ship->id;
      ctx.colony_id = best_colony;

      std::string msg = "Ship repaired: " + ship->name;
      if (colony) msg += " at " + colony->name;
      if (sys) msg += " in " + sys->name;
      push_event(EventLevel::Info, EventCategory::Shipyard, std::move(msg), ctx);
    }
  }
}

} // namespace nebula4x
