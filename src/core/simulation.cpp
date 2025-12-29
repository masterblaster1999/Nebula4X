#include "nebula4x/core/simulation.h"

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

namespace nebula4x {
namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

std::string ascii_to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

bool is_mining_installation(const InstallationDef& def) {
  if (def.mining) return true;
  // Back-compat heuristic: if the content didn't explicitly set the flag,
  // treat installations whose id contains "mine" and that produce minerals as miners.
  if (def.produces_per_day.empty()) return false;
  const std::string lid = ascii_to_lower(def.id);
  return lid.find("mine") != std::string::npos;
}

double mkm_per_day_from_speed(double speed_km_s, double seconds_per_day) {
  const double km_per_day = speed_km_s * seconds_per_day;
  return km_per_day / 1.0e6; // million km
}

template <typename T>
void push_unique(std::vector<T>& v, const T& x) {
  if (std::find(v.begin(), v.end(), x) == v.end()) v.push_back(x);
}

bool vec_contains(const std::vector<std::string>& v, const std::string& x) {
  return std::find(v.begin(), v.end(), x) != v.end();
}

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

std::optional<JumpRoutePlan> compute_jump_route_plan(const Simulation& sim, Id start_system_id,
                                                    Vec2 start_pos_mkm, Id faction_id,
                                                    double speed_km_s, Id target_system_id,
                                                    bool restrict_to_discovered) {
  const auto& s = sim.state();

  if (!find_ptr(s.systems, start_system_id)) return std::nullopt;
  if (!find_ptr(s.systems, target_system_id)) return std::nullopt;

  auto allow_system = [&](Id sys_id) {
    if (!restrict_to_discovered) return true;
    return sim.is_system_discovered_by_faction(faction_id, sys_id);
  };

  if (restrict_to_discovered && !allow_system(target_system_id)) return std::nullopt;

  if (start_system_id == target_system_id) {
    JumpRoutePlan plan;
    plan.systems = {start_system_id};
    plan.distance_mkm = 0.0;
    plan.eta_days = 0.0;
    return plan;
  }

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

    std::vector<Id> outgoing = sys->jump_points;
    std::sort(outgoing.begin(), outgoing.end());
    outgoing.erase(std::unique(outgoing.begin(), outgoing.end()), outgoing.end());

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

bool faction_has_tech(const Faction& f, const std::string& tech_id) {
  return std::find(f.known_techs.begin(), f.known_techs.end(), tech_id) != f.known_techs.end();
}

struct SensorSource {
  Vec2 pos_mkm{0.0, 0.0};
  double range_mkm{0.0};
};

// Simple per-design power allocation used for load shedding.
//
// Priority order (highest to lowest): engines -> shields -> weapons -> sensors.
// If available reactor power is insufficient, lower-priority subsystems go
// offline. This is intentionally simple/deterministic for the prototype.
struct PowerAllocation {
  double generation{0.0};
  double available{0.0};
  bool engines_online{true};
  bool shields_online{true};
  bool weapons_online{true};
  bool sensors_online{true};
};

PowerAllocation compute_power_allocation(const ShipDesign& d) {
  PowerAllocation out;
  out.generation = std::max(0.0, d.power_generation);
  double avail = out.generation;

  auto on = [&](double req) {
    req = std::max(0.0, req);
    if (req <= 1e-9) return true;
    if (req <= avail + 1e-9) {
      avail -= req;
      return true;
    }
    return false;
  };

  out.engines_online = on(d.power_use_engines);
  out.shields_online = on(d.power_use_shields);
  out.weapons_online = on(d.power_use_weapons);
  out.sensors_online = on(d.power_use_sensors);

  out.available = avail;
  return out;
}

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
      const auto p = compute_power_allocation(*d);
      if (!p.sensors_online && d->power_use_sensors > 1e-9) {
        range = 0.0;
      }
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

// Many core containers are stored as std::unordered_map for convenience.
// Iteration order of unordered_map is not specified, so relying on it can
// introduce cross-platform nondeterminism.
template <typename Map>
std::vector<typename Map::key_type> sorted_keys(const Map& m) {
  std::vector<typename Map::key_type> keys;
  keys.reserve(m.size());
  for (const auto& [k, _] : m) keys.push_back(k);
  std::sort(keys.begin(), keys.end());
  return keys;
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

  return total;
}

std::vector<LogisticsNeed> Simulation::logistics_needs_for_faction(Id faction_id) const {
  std::vector<LogisticsNeed> out;
  if (faction_id == kInvalidId) return out;

  const InstallationDef* shipyard_def = nullptr;
  if (auto it = content_.installations.find("shipyard"); it != content_.installations.end()) {
    shipyard_def = &it->second;
  }

  const auto colony_ids = sorted_keys(state_.colonies);
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
        const double capacity_tons = shipyard_def->build_rate_tons_per_day * static_cast<double>(yards);
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
  }

  return out;
}

bool Simulation::is_system_discovered_by_faction(Id viewer_faction_id, Id system_id) const {
  const auto* fac = find_ptr(state_.factions, viewer_faction_id);
  if (!fac) return true;
  return std::find(fac->discovered_systems.begin(), fac->discovered_systems.end(), system_id) !=
         fac->discovered_systems.end();
}

std::optional<JumpRoutePlan> Simulation::plan_jump_route_for_ship(Id ship_id, Id target_system_id,
                                                                 bool restrict_to_discovered,
                                                                 bool include_queued_jumps) const {
  const auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return std::nullopt;

  const PredictedNavState nav = predicted_nav_state_after_queued_jumps(state_, ship_id, include_queued_jumps);
  if (nav.system_id == kInvalidId) return std::nullopt;

  return compute_jump_route_plan(*this, nav.system_id, nav.position_mkm, ship->faction_id, ship->speed_km_s,
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

  return compute_jump_route_plan(*this, nav.system_id, nav.position_mkm, fl->faction_id, slowest, target_system_id,
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
      push_event(EventLevel::Info, EventCategory::General, msg, ctx);
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

  for (Id sid : sys->ships) {
    const auto* sh = find_ptr(state_.ships, sid);
    if (!sh) continue;
    if (sh->faction_id == viewer_faction_id) continue;
    if (!are_factions_hostile(viewer_faction_id, sh->faction_id)) continue;
    if (any_source_detects(sources, sh->position_mkm)) out.push_back(sid);
  }

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
  so.repeat_template.clear();
  return true;
}

bool Simulation::enable_order_repeat(Id ship_id) {
  if (!find_ptr(state_.ships, ship_id)) return false;
  auto& so = state_.ship_orders[ship_id];
  if (so.queue.empty()) return false;
  so.repeat = true;
  so.repeat_template = so.queue;
  return true;
}

bool Simulation::update_order_repeat_template(Id ship_id) {
  if (!find_ptr(state_.ships, ship_id)) return false;
  auto& so = state_.ship_orders[ship_id];
  if (so.queue.empty()) return false;
  so.repeat = true;
  so.repeat_template = so.queue;
  return true;
}

bool Simulation::disable_order_repeat(Id ship_id) {
  if (!find_ptr(state_.ships, ship_id)) return false;
  auto& so = state_.ship_orders[ship_id];
  so.repeat = false;
  so.repeat_template.clear();
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

  const auto plan = compute_jump_route_plan(*this, leader_nav.system_id, leader_nav.position_mkm,
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

  const auto plan = compute_jump_route_plan(*this, nav.system_id, nav.position_mkm, ship->faction_id, ship->speed_km_s,
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
      const double period = std::max(1.0, b.orbit_period_days);
      const double theta = b.orbit_phase_radians + kTwoPi * (t / period);
      pos = center + Vec2{b.orbit_radius_mkm * std::cos(theta), b.orbit_radius_mkm * std::sin(theta)};
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
  tick_combat();
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

  const auto ship_ids = sorted_keys(state_.ships);
  const auto faction_ids = sorted_keys(state_.factions);

  for (Id ship_id : ship_ids) {
    const auto* sh = find_ptr(state_.ships, ship_id);
    if (!sh) continue;

    for (Id fid : faction_ids) {
      auto* fac = find_ptr(state_.factions, fid);
      if (!fac) continue;
      if (fac->id == sh->faction_id) continue;

      const auto& sources = sources_for(fac->id, sh->system_id);
      if (sources.empty()) continue;
      if (!any_source_detects(sources, sh->position_mkm)) continue;

      detected_today_by_faction[fac->id].push_back(ship_id);

      bool is_new = false;
      bool was_stale = false;
      if (auto it = fac->ship_contacts.find(ship_id); it == fac->ship_contacts.end()) {
        is_new = true;
      } else {
        was_stale = (it->second.last_seen_day < now - 1);
      }

      Contact c;
      c.ship_id = ship_id;
      c.system_id = sh->system_id;
      c.last_seen_day = now;
      c.last_seen_position_mkm = sh->position_mkm;
      c.last_seen_name = sh->name;
      c.last_seen_design_id = sh->design_id;
      c.last_seen_faction_id = sh->faction_id;
      fac->ship_contacts[ship_id] = std::move(c);

      if (is_new || was_stale) {
        const auto* sys = find_ptr(state_.systems, sh->system_id);
        const std::string sys_name = sys ? sys->name : std::string("(unknown)");
        const auto* other_f = find_ptr(state_.factions, sh->faction_id);
        const std::string other_name = other_f ? other_f->name : std::string("(unknown)");

        EventContext ctx;
        ctx.faction_id = fac->id;
        ctx.faction_id2 = sh->faction_id;
        ctx.system_id = sh->system_id;
        ctx.ship_id = ship_id;

        std::string msg;
        if (is_new) {
          msg = "New contact for " + fac->name + ": " + sh->name + " (" + other_name + ") in " + sys_name;
        } else {
          msg = "Contact reacquired for " + fac->name + ": " + sh->name + " (" + other_name + ") in " + sys_name;
        }

        push_event(EventLevel::Info, EventCategory::Intel, msg, ctx);
      }
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

    const auto p = compute_power_allocation(*d);

    const double max_sh = std::max(0.0, d->max_shields);
    if (max_sh <= 1e-9) {
      sh->shields = 0.0;
      continue;
    }

    // If shields draw power and are offline, treat them as fully down.
    if (!p.shields_online && d->power_use_shields > 1e-9) {
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
  // Aggregate mining requests so that multiple colonies on the same body share
  // finite deposits fairly (proportional allocation) and deterministically.
  //
  // Structure: body_id -> mineral -> [(colony_id, requested_tons_per_day), ...]
  std::unordered_map<Id, std::unordered_map<std::string, std::vector<std::pair<Id, double>>>> mine_reqs;
  mine_reqs.reserve(state_.colonies.size());

  for (Id cid : sorted_keys(state_.colonies)) {
    auto& colony = state_.colonies.at(cid);

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
            colony.minerals[mineral] += per_day * static_cast<double>(count);
          }
          continue;
        }
        for (const auto& [mineral, per_day] : def.produces_per_day) {
          const double req = per_day * static_cast<double>(count);
          if (req <= 1e-12) continue;
          mine_reqs[body_id][mineral].push_back({cid, req});
        }
      } else {
        // Synthetic production: add directly to colony stockpile.
        for (const auto& [mineral, per_day] : def.produces_per_day) {
          colony.minerals[mineral] += per_day * static_cast<double>(count);
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
}

void Simulation::tick_research() {
  for (Id cid : sorted_keys(state_.colonies)) {
    auto& col = state_.colonies.at(cid);
    double rp_per_day = 0.0;
    for (const auto& [inst_id, count] : col.installations) {
      const auto dit = content_.installations.find(inst_id);
      if (dit == content_.installations.end()) continue;
      rp_per_day += dit->second.research_points_per_day * static_cast<double>(count);
    }
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

    double capacity_tons = base_rate * static_cast<double>(yards);

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
    return it->second.queue.empty();
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

    auto plan = compute_jump_route_plan(*this, start_system_id, start_pos_mkm, fid, speed_km_s, goal_system_id,
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

    struct NeedAgg {
      Id colony_id{kInvalidId};
      std::string mineral;
      double missing_tons{0.0};
    };

    std::vector<NeedAgg> need_list;
    for (Id cid : colony_ids) {
      auto it = missing_by_colony.find(cid);
      if (it == missing_by_colony.end()) continue;
      std::vector<std::string> minerals;
      minerals.reserve(it->second.size());
      for (const auto& [mineral, _] : it->second) minerals.push_back(mineral);
      std::sort(minerals.begin(), minerals.end());
      for (const auto& mineral : minerals) {
        need_list.push_back(NeedAgg{cid, mineral, it->second[mineral]});
      }
    }

    std::sort(need_list.begin(), need_list.end(), [](const NeedAgg& a, const NeedAgg& b) {
      if (a.missing_tons != b.missing_tons) return a.missing_tons > b.missing_tons;
      if (a.colony_id != b.colony_id) return a.colony_id < b.colony_id;
      return a.mineral < b.mineral;
    });

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

      // 1) If we already have cargo, try to deliver it to the nearest colony that needs it.
      bool assigned = false;
      if (used > 1e-9 && !need_list.empty()) {
        for (const auto& [mineral, tons_raw] : sh->cargo) {
          const double tons = std::max(0.0, tons_raw);
          if (tons <= 1e-9) continue;

          int best_idx = -1;
          double best_eta = std::numeric_limits<double>::infinity();
          double best_missing = 0.0;

          for (int i = 0; i < static_cast<int>(need_list.size()); ++i) {
            if (need_list[i].missing_tons <= 1e-9) continue;
            if (need_list[i].mineral != mineral) continue;

            const Id dcid = need_list[i].colony_id;
            auto it_sys = colony_system.find(dcid);
            auto it_pos = colony_pos.find(dcid);
            if (it_sys == colony_system.end() || it_pos == colony_pos.end()) continue;

            const double eta = estimate_eta_days_to_pos(sh->system_id, sh->position_mkm, fid, sh->speed_km_s,
                                                       it_sys->second, it_pos->second);

            const double miss = need_list[i].missing_tons;
            if (best_idx < 0 || eta < best_eta - 1e-9 ||
                (std::abs(eta - best_eta) <= 1e-9 && (miss > best_missing + 1e-9 ||
                                                     (std::abs(miss - best_missing) <= 1e-9 &&
                                                      dcid < need_list[best_idx].colony_id)))) {
              best_idx = i;
              best_eta = eta;
              best_missing = miss;
            }
          }

          if (best_idx >= 0) {
            const Id dest_cid = need_list[best_idx].colony_id;
            const double amount = std::min(tons, need_list[best_idx].missing_tons);
            if (amount > 1e-9 && issue_unload_mineral(sid, dest_cid, mineral, amount, /*restrict_to_discovered=*/true)) {
              need_list[best_idx].missing_tons = std::max(0.0, need_list[best_idx].missing_tons - amount);
              assigned = true;
              break;
            }
          }
        }
      }
      if (assigned) continue;

      // 2) Otherwise, pick a source colony and a destination need.
      if (free <= 1e-9) continue;
      if (need_list.empty()) continue;

      struct Choice {
        Id source_colony{kInvalidId};
        Id dest_colony{kInvalidId};
        std::string mineral;
        double amount{0.0};
        double eta_total{std::numeric_limits<double>::infinity()};
      } best;

      for (const auto& need : need_list) {
        if (need.missing_tons <= 1e-9) continue;
        if (need.missing_tons < cfg_.auto_freight_min_transfer_tons) continue;

        const Id dest_cid = need.colony_id;
        auto it_dest_sys = colony_system.find(dest_cid);
        auto it_dest_pos = colony_pos.find(dest_cid);
        if (it_dest_sys == colony_system.end() || it_dest_pos == colony_pos.end()) continue;

        for (Id scid : colony_ids) {
          if (scid == dest_cid) continue;
          auto it_exp_c = exportable_by_colony.find(scid);
          if (it_exp_c == exportable_by_colony.end()) continue;
          auto it_exp = it_exp_c->second.find(need.mineral);
          if (it_exp == it_exp_c->second.end()) continue;

          const double avail = std::max(0.0, it_exp->second);
          if (avail <= 1e-9) continue;

          const double take_cap = avail * std::clamp(cfg_.auto_freight_max_take_fraction_of_surplus, 0.0, 1.0);
          const double amount = std::min({free, need.missing_tons, take_cap});
          if (amount < cfg_.auto_freight_min_transfer_tons) continue;

          auto it_src_sys = colony_system.find(scid);
          auto it_src_pos = colony_pos.find(scid);
          if (it_src_sys == colony_system.end() || it_src_pos == colony_pos.end()) continue;

          const double eta1 = estimate_eta_days_to_pos(sh->system_id, sh->position_mkm, fid, sh->speed_km_s,
                                                       it_src_sys->second, it_src_pos->second);
          if (!std::isfinite(eta1)) continue;

          const double eta2 = estimate_eta_days_to_pos(it_src_sys->second, it_src_pos->second, fid, sh->speed_km_s,
                                                       it_dest_sys->second, it_dest_pos->second);
          if (!std::isfinite(eta2)) continue;

          const double eta_total = eta1 + eta2;

          if (best.source_colony == kInvalidId || eta_total < best.eta_total - 1e-9 ||
              (std::abs(eta_total - best.eta_total) <= 1e-9 && (amount > best.amount + 1e-9 ||
                                                               (std::abs(amount - best.amount) <= 1e-9 &&
                                                                (dest_cid < best.dest_colony ||
                                                                 (dest_cid == best.dest_colony && scid < best.source_colony)))))) {
            best.source_colony = scid;
            best.dest_colony = dest_cid;
            best.mineral = need.mineral;
            best.amount = amount;
            best.eta_total = eta_total;
          }
        }
      }

      if (best.source_colony != kInvalidId && best.dest_colony != kInvalidId &&
          best.amount >= cfg_.auto_freight_min_transfer_tons) {
        if (issue_load_mineral(sid, best.source_colony, best.mineral, best.amount, /*restrict_to_discovered=*/true)) {
          if (!issue_unload_mineral(sid, best.dest_colony, best.mineral, best.amount, /*restrict_to_discovered=*/true)) {
            // Safety: don't leave half-issued tasks in the queue.
            (void)clear_orders(sid);
          } else {
            // Reduce local bookkeeping so later ships don't over-assign.
            if (auto it = exportable_by_colony.find(best.source_colony); it != exportable_by_colony.end()) {
              auto it2 = it->second.find(best.mineral);
              if (it2 != it->second.end()) {
                it2->second = std::max(0.0, it2->second - best.amount);
              }
            }
            for (auto& need : need_list) {
              if (need.colony_id == best.dest_colony && need.mineral == best.mineral) {
                need.missing_tons = std::max(0.0, need.missing_tons - best.amount);
                break;
              }
            }
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


void Simulation::tick_ships() {
  auto cargo_used_tons = [](const Ship& s) {
    double used = 0.0;
    for (const auto& [_, tons] : s.cargo) used += std::max(0.0, tons);
    return used;
  };

  const double arrive_eps = std::max(0.0, cfg_.arrival_epsilon_mkm);
  const double dock_range = std::max(arrive_eps, cfg_.docking_range_mkm);

  const auto ship_ids = sorted_keys(state_.ships);

  // --- Fleet cohesion prepass ---
  //
  // Fleets are intentionally lightweight in the data model, so we do a small
  // amount of per-tick work here to make fleet-issued orders behave more like
  // a coordinated group.
  //
  // 1) Speed matching: ships in the same fleet executing the same current
  //    movement order will match the slowest ship.
  // 2) Coordinated jump transits: ships in the same fleet attempting to transit
  //    the same jump point in the same system will wait until all have arrived.
  // 3) Formations: fleets may optionally offset per-ship targets for some
  //    cohorts so that ships travel/attack in a loose formation instead of
  //    piling onto the exact same coordinates.

  std::unordered_map<Id, Id> ship_to_fleet;
  ship_to_fleet.reserve(state_.ships.size() * 2);

  if (!state_.fleets.empty()) {
    const auto fleet_ids = sorted_keys(state_.fleets);
    for (Id fid : fleet_ids) {
      const auto* fl = find_ptr(state_.fleets, fid);
      if (!fl) continue;
      for (Id sid : fl->ship_ids) {
        if (sid == kInvalidId) continue;
        ship_to_fleet[sid] = fid;
      }
    }
  }

  enum class CohortKind : std::uint8_t {
    MovePoint,
    MoveBody,
    OrbitBody,
    Jump,
    Attack,
    Load,
    Unload,
    Transfer,
    Scrap,
  };

  struct CohortKey {
    Id fleet_id{kInvalidId};
    Id system_id{kInvalidId};
    CohortKind kind{CohortKind::MovePoint};
    Id target_id{kInvalidId};
    std::uint64_t x_bits{0};
    std::uint64_t y_bits{0};

    bool operator==(const CohortKey& o) const {
      return fleet_id == o.fleet_id && system_id == o.system_id && kind == o.kind && target_id == o.target_id &&
             x_bits == o.x_bits && y_bits == o.y_bits;
    }
  };

  struct CohortKeyHash {
    size_t operator()(const CohortKey& k) const {
      // FNV-1a style mixing.
      std::uint64_t h = 1469598103934665603ull;
      auto mix = [&](std::uint64_t v) {
        h ^= v;
        h *= 1099511628211ull;
      };
      mix(k.fleet_id);
      mix(k.system_id);
      mix(static_cast<std::uint64_t>(k.kind));
      mix(k.target_id);
      mix(k.x_bits);
      mix(k.y_bits);
      return static_cast<size_t>(h);
    }
  };

  auto double_bits = [](double v) -> std::uint64_t {
    std::uint64_t out = 0;
    std::memcpy(&out, &v, sizeof(out));
    return out;
  };

  auto make_cohort_key = [&](Id fleet_id, Id system_id, const Order& ord) -> std::optional<CohortKey> {
    if (fleet_id == kInvalidId) return std::nullopt;

    CohortKey k;
    k.fleet_id = fleet_id;
    k.system_id = system_id;

    if (std::holds_alternative<MoveToPoint>(ord)) {
      k.kind = CohortKind::MovePoint;
      const auto& o = std::get<MoveToPoint>(ord);
      k.x_bits = double_bits(o.target_mkm.x);
      k.y_bits = double_bits(o.target_mkm.y);
      return k;
    }
    if (std::holds_alternative<MoveToBody>(ord)) {
      k.kind = CohortKind::MoveBody;
      k.target_id = std::get<MoveToBody>(ord).body_id;
      return k;
    }
    if (std::holds_alternative<ColonizeBody>(ord)) {
      k.kind = CohortKind::MoveBody;
      k.target_id = std::get<ColonizeBody>(ord).body_id;
      return k;
    }
    if (std::holds_alternative<OrbitBody>(ord)) {
      k.kind = CohortKind::OrbitBody;
      k.target_id = std::get<OrbitBody>(ord).body_id;
      return k;
    }
    if (std::holds_alternative<TravelViaJump>(ord)) {
      k.kind = CohortKind::Jump;
      k.target_id = std::get<TravelViaJump>(ord).jump_point_id;
      return k;
    }
    if (std::holds_alternative<AttackShip>(ord)) {
      k.kind = CohortKind::Attack;
      k.target_id = std::get<AttackShip>(ord).target_ship_id;
      return k;
    }
    if (std::holds_alternative<LoadMineral>(ord)) {
      k.kind = CohortKind::Load;
      k.target_id = std::get<LoadMineral>(ord).colony_id;
      return k;
    }
    if (std::holds_alternative<UnloadMineral>(ord)) {
      k.kind = CohortKind::Unload;
      k.target_id = std::get<UnloadMineral>(ord).colony_id;
      return k;
    }
    if (std::holds_alternative<TransferCargoToShip>(ord)) {
      k.kind = CohortKind::Transfer;
      k.target_id = std::get<TransferCargoToShip>(ord).target_ship_id;
      return k;
    }
    if (std::holds_alternative<ScrapShip>(ord)) {
      k.kind = CohortKind::Scrap;
      k.target_id = std::get<ScrapShip>(ord).colony_id;
      return k;
    }

    return std::nullopt;
  };

  std::unordered_map<CohortKey, double, CohortKeyHash> cohort_min_speed_km_s;

  if (cfg_.fleet_speed_matching && !ship_to_fleet.empty()) {
    cohort_min_speed_km_s.reserve(state_.ships.size() * 2);

    for (Id ship_id : ship_ids) {
      const auto* sh = find_ptr(state_.ships, ship_id);
      if (!sh) continue;

      const auto it_fleet = ship_to_fleet.find(ship_id);
      if (it_fleet == ship_to_fleet.end()) continue;

      const auto it_orders = state_.ship_orders.find(ship_id);
      if (it_orders == state_.ship_orders.end()) continue;

      const ShipOrders& so = it_orders->second;
      const Order* ord_ptr = nullptr;
      if (!so.queue.empty()) {
        ord_ptr = &so.queue.front();
      } else if (so.repeat && !so.repeat_template.empty()) {
        // Mirror the main tick loop behaviour where empty queues are refilled
        // from the repeat template.
        ord_ptr = &so.repeat_template.front();
      } else {
        continue;
      }

      const Order& ord = *ord_ptr;
      if (std::holds_alternative<WaitDays>(ord)) continue;

      const auto key_opt = make_cohort_key(it_fleet->second, sh->system_id, ord);
      if (!key_opt) continue;

      // Power gating for fleet speed matching: if a ship cannot power its
      // engines, treat its speed as 0 for cohesion purposes.
      double base_speed_km_s = sh->speed_km_s;
      if (const auto* sd = find_design(sh->design_id)) {
        const auto p = compute_power_allocation(*sd);
        if (!p.engines_online && sd->power_use_engines > 1e-9) {
          base_speed_km_s = 0.0;
        }
      }

      const CohortKey key = *key_opt;
      auto it_min = cohort_min_speed_km_s.find(key);
      if (it_min == cohort_min_speed_km_s.end()) {
        cohort_min_speed_km_s.emplace(key, base_speed_km_s);
      } else {
        it_min->second = std::min(it_min->second, base_speed_km_s);
      }
    }
  }

  struct JumpGroupKey {
    Id fleet_id{kInvalidId};
    Id jump_id{kInvalidId};
    Id system_id{kInvalidId};

    bool operator==(const JumpGroupKey& o) const {
      return fleet_id == o.fleet_id && jump_id == o.jump_id && system_id == o.system_id;
    }
  };

  struct JumpGroupKeyHash {
    size_t operator()(const JumpGroupKey& k) const {
      std::uint64_t h = 1469598103934665603ull;
      auto mix = [&](std::uint64_t v) {
        h ^= v;
        h *= 1099511628211ull;
      };
      mix(k.fleet_id);
      mix(k.jump_id);
      mix(k.system_id);
      return static_cast<size_t>(h);
    }
  };

  struct JumpGroupState {
    int count{0};
    bool valid{false};
    bool ready{false};
    Vec2 jp_pos{0.0, 0.0};
  };

  std::unordered_map<JumpGroupKey, JumpGroupState, JumpGroupKeyHash> jump_group_state;

  if (cfg_.fleet_coordinated_jumps && !ship_to_fleet.empty()) {
    std::unordered_map<JumpGroupKey, std::vector<Id>, JumpGroupKeyHash> group_members;
    group_members.reserve(state_.fleets.size() * 2);

    for (Id ship_id : ship_ids) {
      const auto* sh = find_ptr(state_.ships, ship_id);
      if (!sh) continue;

      const auto it_fleet = ship_to_fleet.find(ship_id);
      if (it_fleet == ship_to_fleet.end()) continue;

      const auto it_orders = state_.ship_orders.find(ship_id);
      if (it_orders == state_.ship_orders.end()) continue;

      const ShipOrders& so = it_orders->second;
      const Order* ord_ptr = nullptr;
      if (!so.queue.empty()) {
        ord_ptr = &so.queue.front();
      } else if (so.repeat && !so.repeat_template.empty()) {
        ord_ptr = &so.repeat_template.front();
      } else {
        continue;
      }

      const Order& ord = *ord_ptr;
      if (!std::holds_alternative<TravelViaJump>(ord)) continue;

      const Id jump_id = std::get<TravelViaJump>(ord).jump_point_id;
      if (jump_id == kInvalidId) continue;

      JumpGroupKey key;
      key.fleet_id = it_fleet->second;
      key.jump_id = jump_id;
      key.system_id = sh->system_id;
      group_members[key].push_back(ship_id);
    }

    jump_group_state.reserve(group_members.size() * 2);

    for (auto& [key, members] : group_members) {
      JumpGroupState st;
      st.count = static_cast<int>(members.size());

      const auto* jp = find_ptr(state_.jump_points, key.jump_id);
      if (jp && jp->system_id == key.system_id) {
        st.valid = true;
        st.jp_pos = jp->position_mkm;
        if (st.count > 1) {
          bool ready = true;
          for (Id sid : members) {
            const auto* s2 = find_ptr(state_.ships, sid);
            if (!s2) {
              ready = false;
              break;
            }
            const double dist = (s2->position_mkm - st.jp_pos).length();
            if (dist > dock_range + 1e-9) {
              ready = false;
              break;
            }
          }
          st.ready = ready;
        }
      }

      jump_group_state.emplace(key, st);
    }
  }

  // Fleet formation offsets (optional).
  //
  // This is intentionally lightweight: we only compute offsets for cohorts
  // where a formation makes sense (currently: move-to-point and attack).
  std::unordered_map<Id, Vec2> formation_offset_mkm;
  if (cfg_.fleet_formations && !ship_to_fleet.empty()) {
    std::unordered_map<CohortKey, std::vector<Id>, CohortKeyHash> cohorts;
    cohorts.reserve(state_.fleets.size() * 2);

    for (Id ship_id : ship_ids) {
      const auto* sh = find_ptr(state_.ships, ship_id);
      if (!sh) continue;

      const auto it_fleet = ship_to_fleet.find(ship_id);
      if (it_fleet == ship_to_fleet.end()) continue;

      const auto* fl = find_ptr(state_.fleets, it_fleet->second);
      if (!fl) continue;
      if (fl->formation == FleetFormation::None) continue;
      if (fl->formation_spacing_mkm <= 0.0) continue;

      const auto it_orders = state_.ship_orders.find(ship_id);
      if (it_orders == state_.ship_orders.end()) continue;

      const ShipOrders& so = it_orders->second;
      const Order* ord_ptr = nullptr;
      if (!so.queue.empty()) {
        ord_ptr = &so.queue.front();
      } else if (so.repeat && !so.repeat_template.empty()) {
        ord_ptr = &so.repeat_template.front();
      } else {
        continue;
      }

      const Order& ord = *ord_ptr;
      if (std::holds_alternative<WaitDays>(ord)) continue;

      const auto key_opt = make_cohort_key(it_fleet->second, sh->system_id, ord);
      if (!key_opt) continue;
      const CohortKey key = *key_opt;
      if (key.kind != CohortKind::MovePoint && key.kind != CohortKind::Attack) continue;

      cohorts[key].push_back(ship_id);
    }

    formation_offset_mkm.reserve(state_.ships.size() * 2);

    auto bits_to_double = [](std::uint64_t bits) -> double {
      double out = 0.0;
      std::memcpy(&out, &bits, sizeof(out));
      return out;
    };

    for (auto& [key, members] : cohorts) {
      if (members.size() < 2) continue;
      std::sort(members.begin(), members.end());
      members.erase(std::unique(members.begin(), members.end()), members.end());
      if (members.size() < 2) continue;

      const auto* fl = find_ptr(state_.fleets, key.fleet_id);
      if (!fl) continue;
      if (fl->formation == FleetFormation::None) continue;

      const double spacing = std::max(0.0, fl->formation_spacing_mkm);
      if (spacing <= 0.0) continue;

      Id leader_id = fl->leader_ship_id;
      if (leader_id == kInvalidId || std::find(members.begin(), members.end(), leader_id) == members.end()) {
        leader_id = members.front();
      }

      const auto* leader = find_ptr(state_.ships, leader_id);
      if (!leader) continue;
      const Vec2 leader_pos = leader->position_mkm;

      Vec2 raw_target = leader_pos + Vec2{1.0, 0.0};
      if (key.kind == CohortKind::MovePoint) {
        raw_target = Vec2{bits_to_double(key.x_bits), bits_to_double(key.y_bits)};
      } else if (key.kind == CohortKind::Attack) {
        const Id target_ship_id = key.target_id;
        const bool detected = is_ship_detected_by_faction(leader->faction_id, target_ship_id);
        if (detected) {
          if (const auto* tgt = find_ptr(state_.ships, target_ship_id)) raw_target = tgt->position_mkm;
        } else {
          const Order* lord_ptr = nullptr;
          if (auto itso = state_.ship_orders.find(leader_id); itso != state_.ship_orders.end()) {
            const ShipOrders& so = itso->second;
            if (!so.queue.empty()) {
              lord_ptr = &so.queue.front();
            } else if (so.repeat && !so.repeat_template.empty()) {
              lord_ptr = &so.repeat_template.front();
            }
          }
          if (lord_ptr && std::holds_alternative<AttackShip>(*lord_ptr)) {
            const auto& ao = std::get<AttackShip>(*lord_ptr);
            if (ao.has_last_known) raw_target = ao.last_known_position_mkm;
          }
        }
      }

      Vec2 forward = raw_target - leader_pos;
      const double flen = forward.length();
      if (flen < 1e-9) {
        forward = Vec2{1.0, 0.0};
      } else {
        forward = forward * (1.0 / flen);
      }
      const Vec2 right{-forward.y, forward.x};

      auto world_from_local = [&](double x_right, double y_forward) -> Vec2 {
        return right * x_right + forward * y_forward;
      };

      formation_offset_mkm[leader_id] = Vec2{0.0, 0.0};

      std::vector<Id> followers = members;
      followers.erase(std::remove(followers.begin(), followers.end(), leader_id), followers.end());

      const std::size_t m = followers.size();
      if (m == 0) continue;

      for (std::size_t i = 0; i < m; ++i) {
        const Id sid = followers[i];
        Vec2 off{0.0, 0.0};

        switch (fl->formation) {
          case FleetFormation::LineAbreast: {
            const int ring = static_cast<int>(i / 2) + 1;
            const int sign = ((i % 2) == 0) ? 1 : -1;
            off = world_from_local(static_cast<double>(sign * ring) * spacing, 0.0);
            break;
          }
          case FleetFormation::Column: {
            off = world_from_local(0.0, -static_cast<double>(i + 1) * spacing);
            break;
          }
          case FleetFormation::Wedge: {
            const int layer = static_cast<int>(i / 2) + 1;
            const int sign = ((i % 2) == 0) ? 1 : -1;
            off = world_from_local(static_cast<double>(sign * layer) * spacing, -static_cast<double>(layer) * spacing);
            break;
          }
          case FleetFormation::Ring: {
            const double angle = kTwoPi * (static_cast<double>(i) / static_cast<double>(m));
            const double radius = std::max(spacing, (static_cast<double>(m) * spacing) / kTwoPi);
            off = world_from_local(std::cos(angle) * radius, std::sin(angle) * radius);
            break;
          }
          case FleetFormation::None:
          default: {
            off = Vec2{0.0, 0.0};
            break;
          }
        }

        formation_offset_mkm[sid] = off;
      }
    }
  }

  for (Id ship_id : ship_ids) {
    auto it_ship = state_.ships.find(ship_id);
    if (it_ship == state_.ships.end()) continue;
    auto& ship = it_ship->second;

    const Id fleet_id = [&]() -> Id {
      const auto it_fleet = ship_to_fleet.find(ship_id);
      return (it_fleet != ship_to_fleet.end()) ? it_fleet->second : kInvalidId;
    }();

    auto it = state_.ship_orders.find(ship_id);
    if (it == state_.ship_orders.end()) continue;
    auto& so = it->second;

    if (so.queue.empty() && so.repeat && !so.repeat_template.empty()) {
      so.queue = so.repeat_template;
    }

    auto& q = so.queue;
    if (q.empty()) continue;

    if (std::holds_alternative<WaitDays>(q.front())) {
      auto& ord = std::get<WaitDays>(q.front());
      if (ord.days_remaining <= 0) {
        q.erase(q.begin());
        continue;
      }
      ord.days_remaining -= 1;
      if (ord.days_remaining <= 0) q.erase(q.begin());
      continue;
    }

    Vec2 target = ship.position_mkm;
    double desired_range = 0.0; 
    bool attack_has_contact = false;

    // Cargo vars
    bool is_cargo_op = false;
    // 0=Load, 1=Unload, 2=TransferToShip
    int cargo_mode = 0; 

    // Pointers to active orders for updating state
    LoadMineral* load_ord = nullptr;
    UnloadMineral* unload_ord = nullptr;
    TransferCargoToShip* transfer_ord = nullptr;

    Id cargo_colony_id = kInvalidId;
    Id cargo_target_ship_id = kInvalidId;
    std::string cargo_mineral;
    double cargo_tons = 0.0;

    if (std::holds_alternative<MoveToPoint>(q.front())) {
      target = std::get<MoveToPoint>(q.front()).target_mkm;
    } else if (std::holds_alternative<MoveToBody>(q.front())) {
      const Id body_id = std::get<MoveToBody>(q.front()).body_id;
      const auto* body = find_ptr(state_.bodies, body_id);
      if (!body) {
        q.erase(q.begin());
        continue;
      }
      if (body->system_id != ship.system_id) {
        q.erase(q.begin());
        continue;
      }
      target = body->position_mkm;
    } else if (std::holds_alternative<ColonizeBody>(q.front())) {
      const auto& ord = std::get<ColonizeBody>(q.front());
      const Id body_id = ord.body_id;
      const auto* body = find_ptr(state_.bodies, body_id);
      if (!body) {
        q.erase(q.begin());
        continue;
      }
      if (body->system_id != ship.system_id) {
        q.erase(q.begin());
        continue;
      }
      target = body->position_mkm;
    } else if (std::holds_alternative<OrbitBody>(q.front())) {
      auto& ord = std::get<OrbitBody>(q.front());
      const Id body_id = ord.body_id;
      const auto* body = find_ptr(state_.bodies, body_id);
      if (!body || body->system_id != ship.system_id) {
        q.erase(q.begin());
        continue;
      }
      target = body->position_mkm;
    } else if (std::holds_alternative<TravelViaJump>(q.front())) {
      const Id jump_id = std::get<TravelViaJump>(q.front()).jump_point_id;
      const auto* jp = find_ptr(state_.jump_points, jump_id);
      if (!jp || jp->system_id != ship.system_id) {
        q.erase(q.begin());
        continue;
      }
      target = jp->position_mkm;
    } else if (std::holds_alternative<AttackShip>(q.front())) {
      auto& ord = std::get<AttackShip>(q.front());
      const Id target_id = ord.target_ship_id;
      const auto* tgt = find_ptr(state_.ships, target_id);
      if (!tgt || tgt->system_id != ship.system_id) {
        q.erase(q.begin());
        continue;
      }

      attack_has_contact = is_ship_detected_by_faction(ship.faction_id, target_id);
      // An explicit AttackShip order acts as a de-facto declaration of hostilities if needed.
      if (attack_has_contact && !are_factions_hostile(ship.faction_id, tgt->faction_id)) {
        set_diplomatic_status(ship.faction_id, tgt->faction_id, DiplomacyStatus::Hostile, /*reciprocal=*/true,
                             /*push_event_on_change=*/true);
      }


      if (attack_has_contact) {
        target = tgt->position_mkm;
        ord.last_known_position_mkm = target;
        ord.has_last_known = true;
        const auto* d = find_design(ship.design_id);
        const double w_range = d ? d->weapon_range_mkm : 0.0;
        desired_range = (w_range > 0.0) ? (w_range * 0.9) : 0.1;
      } else {
        if (!ord.has_last_known) {
          q.erase(q.begin());
          continue;
        }
        target = ord.last_known_position_mkm;
        desired_range = 0.0;
      }
    } else if (std::holds_alternative<LoadMineral>(q.front())) {
      auto& ord = std::get<LoadMineral>(q.front());
      is_cargo_op = true;
      cargo_mode = 0;
      load_ord = &ord;
      cargo_colony_id = ord.colony_id;
      cargo_mineral = ord.mineral;
      cargo_tons = ord.tons;
      const auto* colony = find_ptr(state_.colonies, cargo_colony_id);
      if (!colony || colony->faction_id != ship.faction_id) { q.erase(q.begin()); continue; }
      const auto* body = find_ptr(state_.bodies, colony->body_id);
      if (!body || body->system_id != ship.system_id) { q.erase(q.begin()); continue; }
      target = body->position_mkm;
    } else if (std::holds_alternative<UnloadMineral>(q.front())) {
      auto& ord = std::get<UnloadMineral>(q.front());
      is_cargo_op = true;
      cargo_mode = 1;
      unload_ord = &ord;
      cargo_colony_id = ord.colony_id;
      cargo_mineral = ord.mineral;
      cargo_tons = ord.tons;
      const auto* colony = find_ptr(state_.colonies, cargo_colony_id);
      if (!colony || colony->faction_id != ship.faction_id) { q.erase(q.begin()); continue; }
      const auto* body = find_ptr(state_.bodies, colony->body_id);
      if (!body || body->system_id != ship.system_id) { q.erase(q.begin()); continue; }
      target = body->position_mkm;
    } else if (std::holds_alternative<TransferCargoToShip>(q.front())) {
      auto& ord = std::get<TransferCargoToShip>(q.front());
      is_cargo_op = true;
      cargo_mode = 2;
      transfer_ord = &ord;
      cargo_target_ship_id = ord.target_ship_id;
      cargo_mineral = ord.mineral;
      cargo_tons = ord.tons;
      const auto* tgt = find_ptr(state_.ships, cargo_target_ship_id);
      // Valid target check: exists, same system, same faction
      if (!tgt || tgt->system_id != ship.system_id || tgt->faction_id != ship.faction_id) {
        q.erase(q.begin());
        continue;
      }
      target = tgt->position_mkm;
    } else if (std::holds_alternative<ScrapShip>(q.front())) {
      auto& ord = std::get<ScrapShip>(q.front());
      const auto* colony = find_ptr(state_.colonies, ord.colony_id);
      if (!colony || colony->faction_id != ship.faction_id) { q.erase(q.begin()); continue; }
      const auto* body = find_ptr(state_.bodies, colony->body_id);
      if (!body || body->system_id != ship.system_id) { q.erase(q.begin()); continue; }
      target = body->position_mkm;
    }

    // Fleet formation: optionally offset the movement/attack target.
    if (cfg_.fleet_formations && fleet_id != kInvalidId && !formation_offset_mkm.empty()) {
      const bool can_offset = std::holds_alternative<MoveToPoint>(q.front()) ||
                              std::holds_alternative<AttackShip>(q.front());
      if (can_offset) {
        if (auto itoff = formation_offset_mkm.find(ship_id); itoff != formation_offset_mkm.end()) {
          target = target + itoff->second;
        }
      }
    }

    const Vec2 delta = target - ship.position_mkm;
    const double dist = delta.length();

    const bool is_attack = std::holds_alternative<AttackShip>(q.front());
    const bool is_jump = std::holds_alternative<TravelViaJump>(q.front());
    const bool is_move_body = std::holds_alternative<MoveToBody>(q.front());
    const bool is_colonize = std::holds_alternative<ColonizeBody>(q.front());
    const bool is_body = is_move_body || is_colonize;
    const bool is_orbit = std::holds_alternative<OrbitBody>(q.front());
    const bool is_scrap = std::holds_alternative<ScrapShip>(q.front());

    // Fleet jump coordination: if multiple ships in the same fleet are trying to
    // transit the same jump point in the same system, we can optionally hold the
    // transit until all of them have arrived.
    bool is_coordinated_jump_group = false;
    bool allow_jump_transit = true;
    if (is_jump && cfg_.fleet_coordinated_jumps && fleet_id != kInvalidId && !jump_group_state.empty()) {
      const Id jump_id = std::get<TravelViaJump>(q.front()).jump_point_id;
      JumpGroupKey key;
      key.fleet_id = fleet_id;
      key.jump_id = jump_id;
      key.system_id = ship.system_id;

      const auto itjg = jump_group_state.find(key);
      if (itjg != jump_group_state.end() && itjg->second.valid && itjg->second.count > 1) {
        is_coordinated_jump_group = true;
        allow_jump_transit = itjg->second.ready;
      }
    }

    auto do_cargo_transfer = [&]() -> double {
      // mode 0=load from col, 1=unload to col, 2=transfer to ship
      
      std::unordered_map<std::string, double>* source_minerals = nullptr;
      std::unordered_map<std::string, double>* dest_minerals = nullptr;
      double dest_capacity_free = 1e300;

      if (cargo_mode == 0) { // Load from colony
        auto* col = find_ptr(state_.colonies, cargo_colony_id);
        if (!col) return 0.0;
        source_minerals = &col->minerals;
        dest_minerals = &ship.cargo;
        
        const auto* d = find_design(ship.design_id);
        const double cap = d ? d->cargo_tons : 0.0;
        dest_capacity_free = std::max(0.0, cap - cargo_used_tons(ship));
      } else if (cargo_mode == 1) { // Unload to colony
        auto* col = find_ptr(state_.colonies, cargo_colony_id);
        if (!col) return 0.0;
        source_minerals = &ship.cargo;
        dest_minerals = &col->minerals;
        // Colony has infinite capacity
      } else if (cargo_mode == 2) { // Transfer to ship
        auto* tgt = find_ptr(state_.ships, cargo_target_ship_id);
        if (!tgt) return 0.0;
        source_minerals = &ship.cargo;
        dest_minerals = &tgt->cargo;
        
        const auto* d = find_design(tgt->design_id);
        const double cap = d ? d->cargo_tons : 0.0;
        dest_capacity_free = std::max(0.0, cap - cargo_used_tons(*tgt));
      }

      if (!source_minerals || !dest_minerals) return 0.0;
      if (dest_capacity_free <= 1e-9) return 0.0;

      double moved_total = 0.0;
      double remaining_request = (cargo_tons > 0.0) ? cargo_tons : 1e300;
      remaining_request = std::min(remaining_request, dest_capacity_free);

      auto transfer_one = [&](const std::string& min_type, double amount_limit) {
        if (amount_limit <= 1e-9) return 0.0;
        auto it_src = source_minerals->find(min_type);
        const double have = (it_src != source_minerals->end()) ? std::max(0.0, it_src->second) : 0.0;
        const double take = std::min(have, amount_limit);
        if (take > 1e-9) {
          (*dest_minerals)[min_type] += take;
          if (it_src != source_minerals->end()) {
            it_src->second = std::max(0.0, it_src->second - take);
            if (it_src->second <= 1e-9) source_minerals->erase(it_src);
          }
          moved_total += take;
        }
        return take;
      };

      if (!cargo_mineral.empty()) {
        transfer_one(cargo_mineral, remaining_request);
        return moved_total;
      }

      std::vector<std::string> keys;
      keys.reserve(source_minerals->size());
      for (const auto& [k, v] : *source_minerals) {
        if (v > 1e-9) keys.push_back(k);
      }
      std::sort(keys.begin(), keys.end());

      for (const auto& k : keys) {
        if (remaining_request <= 1e-9) break;
        const double moved = transfer_one(k, remaining_request);
        remaining_request -= moved;
      }
      return moved_total;
    };

    auto cargo_order_complete = [&](double moved_this_tick) {
      if (cargo_tons <= 0.0) return true; // "As much as possible" -> done after one attempt? No, standard logic usually implies until full/empty.
                                          // But for simplicity, we'll stick to: if we requested unlimited, we try until we can't move anymore.
      
      // Update remaining tons in the order struct
      if (cargo_mode == 0 && load_ord) {
        load_ord->tons = std::max(0.0, load_ord->tons - moved_this_tick);
        cargo_tons = load_ord->tons;
      } else if (cargo_mode == 1 && unload_ord) {
        unload_ord->tons = std::max(0.0, unload_ord->tons - moved_this_tick);
        cargo_tons = unload_ord->tons;
      } else if (cargo_mode == 2 && transfer_ord) {
        transfer_ord->tons = std::max(0.0, transfer_ord->tons - moved_this_tick);
        cargo_tons = transfer_ord->tons;
      }

      if (cargo_tons <= 1e-9) return true;

      // If we couldn't move anything this tick, check if we are blocked (full/empty).
      if (moved_this_tick <= 1e-9) {
          // Simplistic check: if we moved nothing, we are likely done or blocked.
          return true;
      }
      return false;
    };

    // --- Docking / Arrival Checks ---

    if (is_cargo_op && dist <= dock_range) {
      ship.position_mkm = target;
      const double moved = do_cargo_transfer();
      if (cargo_order_complete(moved)) q.erase(q.begin());
      continue;
    }
    if (is_scrap && dist <= dock_range) {
      // Decommission the ship at a friendly colony.
      // - Return carried cargo minerals to the colony stockpile.
      // - Refund a fraction of shipyard mineral costs (estimated by design mass * build_costs_per_ton).
      ship.position_mkm = target;

      const ScrapShip ord = std::get<ScrapShip>(q.front()); // copy (we may erase the ship)
      q.erase(q.begin());

      auto* col = find_ptr(state_.colonies, ord.colony_id);
      if (!col || col->faction_id != ship.faction_id) {
        continue;
      }

      // Snapshot before erasing from state_.
      const Ship ship_snapshot = ship;

      // Return cargo to colony.
      for (const auto& [mineral, tons] : ship_snapshot.cargo) {
        if (tons > 1e-9) col->minerals[mineral] += tons;
      }

      // Return remaining fuel (if any).
      if (ship_snapshot.fuel_tons > 1e-9) col->minerals["Fuel"] += ship_snapshot.fuel_tons;

      // Refund a fraction of shipyard build costs (if configured/content available).
      std::unordered_map<std::string, double> refunded;
      const double refund_frac = std::clamp(cfg_.scrap_refund_fraction, 0.0, 1.0);

      if (refund_frac > 1e-9) {
        const auto it_yard = content_.installations.find("shipyard");
        const auto* design = find_design(ship_snapshot.design_id);
        if (it_yard != content_.installations.end() && design) {
          const double mass_tons = std::max(0.0, design->mass_tons);
          for (const auto& [mineral, per_ton] : it_yard->second.build_costs_per_ton) {
            if (per_ton <= 0.0) continue;
            const double amt = mass_tons * per_ton * refund_frac;
            if (amt > 1e-9) {
              refunded[mineral] += amt;
              col->minerals[mineral] += amt;
            }
          }
        }
      }

      // Remove ship from the system list.
      if (auto* sys = find_ptr(state_.systems, ship_snapshot.system_id)) {
        sys->ships.erase(std::remove(sys->ships.begin(), sys->ships.end(), ship_id), sys->ships.end());
      }

      // Remove ship orders, contacts, and the ship itself.
      state_.ship_orders.erase(ship_id);
      state_.ships.erase(ship_id);

      // Keep fleet membership consistent.
      remove_ship_from_fleets(ship_id);

      for (auto& [_, fac] : state_.factions) {
        fac.ship_contacts.erase(ship_id);
      }

      // Record event.
      {
        std::string msg = "Ship scrapped at " + col->name + ": " + ship_snapshot.name;
        if (!refunded.empty()) {
          std::vector<std::string> keys;
          keys.reserve(refunded.size());
          for (const auto& [k, _] : refunded) keys.push_back(k);
          std::sort(keys.begin(), keys.end());

          msg += " (refund:";
          for (const auto& k : keys) {
            const double v = refunded[k];
            // Print near-integers cleanly.
            if (std::fabs(v - std::round(v)) < 1e-6) {
              msg += " " + k + " " + std::to_string(static_cast<long long>(std::llround(v)));
            } else {
              // Use a compact representation for fractional refunds.
              std::ostringstream ss;
              ss.setf(std::ios::fixed);
              ss.precision(2);
              ss << v;
              msg += " " + k + " " + ss.str();
            }
          }
          msg += ")";
        }

        EventContext ctx;
        ctx.faction_id = col->faction_id;
        ctx.system_id = ship_snapshot.system_id;
        ctx.ship_id = ship_snapshot.id;
        ctx.colony_id = col->id;
        push_event(EventLevel::Info, EventCategory::Shipyard, msg, ctx);
      }

      continue;
    }

    if (is_colonize && dist <= dock_range) {
      ship.position_mkm = target;

      const ColonizeBody ord = std::get<ColonizeBody>(q.front()); // copy (we may erase the ship)
      q.erase(q.begin());

      const auto* body = find_ptr(state_.bodies, ord.body_id);
      if (!body || body->system_id != ship.system_id) {
        continue;
      }

      const bool colonizable = (body->type == BodyType::Planet || body->type == BodyType::Moon ||
                                body->type == BodyType::Asteroid);
      if (!colonizable) {
        EventContext ctx;
        ctx.faction_id = ship.faction_id;
        ctx.system_id = ship.system_id;
        ctx.ship_id = ship_id;
        push_event(EventLevel::Warn, EventCategory::Exploration,
                  "Colonization failed: target body is not colonizable: " + body->name, ctx);
        continue;
      }

      // Ensure the body is not already colonized.
      Id existing_colony_id = kInvalidId;
      std::string existing_colony_name;
      for (const auto& [cid, col] : state_.colonies) {
        if (col.body_id == body->id) {
          existing_colony_id = cid;
          existing_colony_name = col.name;
          break;
        }
      }
      if (existing_colony_id != kInvalidId) {
        EventContext ctx;
        ctx.faction_id = ship.faction_id;
        ctx.system_id = ship.system_id;
        ctx.ship_id = ship_id;
        ctx.colony_id = existing_colony_id;
        push_event(EventLevel::Info, EventCategory::Exploration,
                  "Colonization aborted: " + body->name + " already has a colony (" + existing_colony_name + ")", ctx);
        continue;
      }

      {
        const Ship ship_snapshot = ship;
        const ShipDesign* d = find_design(ship_snapshot.design_id);
        const double cap = d ? d->colony_capacity_millions : 0.0;
        if (cap <= 1e-9) {
          EventContext ctx;
          ctx.faction_id = ship_snapshot.faction_id;
          ctx.system_id = ship_snapshot.system_id;
          ctx.ship_id = ship_snapshot.id;
          push_event(EventLevel::Warn, EventCategory::Exploration,
                    "Colonization failed: ship has no colony module capacity: " + ship_snapshot.name, ctx);
          continue;
        }

        // Choose a unique colony name.
        auto name_exists = [&](const std::string& n) {
          for (const auto& [_, c] : state_.colonies) {
            if (c.name == n) return true;
          }
          return false;
        };
        const std::string base_name = !ord.colony_name.empty() ? ord.colony_name : (body->name + " Colony");
        std::string final_name = base_name;
        for (int suffix = 2; name_exists(final_name); ++suffix) {
          final_name = base_name + " (" + std::to_string(suffix) + ")";
        }

        Colony new_col;
        new_col.id = allocate_id(state_);
        new_col.name = final_name;
        new_col.faction_id = ship_snapshot.faction_id;
        new_col.body_id = body->id;
        new_col.population_millions = cap;

        // Transfer all carried cargo minerals to the new colony.
        for (const auto& [mineral, tons] : ship_snapshot.cargo) {
          if (tons > 1e-9) new_col.minerals[mineral] += tons;
        }

        state_.colonies[new_col.id] = new_col;

        // Ensure the faction has this system discovered.
        if (auto* fac = find_ptr(state_.factions, ship_snapshot.faction_id)) {
          push_unique(fac->discovered_systems, body->system_id);
        }

        // Remove the ship from the system list.
        if (auto* sys = find_ptr(state_.systems, ship_snapshot.system_id)) {
          sys->ships.erase(std::remove(sys->ships.begin(), sys->ships.end(), ship_id), sys->ships.end());
        }

        // Remove ship orders, contacts, and the ship itself.
        state_.ship_orders.erase(ship_id);
        state_.ships.erase(ship_id);

        // Keep fleet membership consistent.
        remove_ship_from_fleets(ship_id);

        for (auto& [_, fac] : state_.factions) {
          fac.ship_contacts.erase(ship_id);
        }

        // Record event.
        {
          std::ostringstream ss;
          ss.setf(std::ios::fixed);
          ss.precision(0);
          ss << cap;
          const std::string msg = "Colony established: " + final_name + " on " + body->name +
                                  " (population " + ss.str() + "M)";
          EventContext ctx;
          ctx.faction_id = new_col.faction_id;
          ctx.system_id = ship_snapshot.system_id;
          ctx.ship_id = ship_snapshot.id;
          ctx.colony_id = new_col.id;
          push_event(EventLevel::Info, EventCategory::Exploration, msg, ctx);
        }
      }

      continue;
    }

    if (is_move_body && dist <= dock_range) {
      ship.position_mkm = target;
      q.erase(q.begin());
      continue;
    }

    if (is_orbit && dist <= dock_range) {
      ship.position_mkm = target; // snap to body
      auto& ord = std::get<OrbitBody>(q.front());
      if (ord.duration_days > 0) {
        ord.duration_days--;
      }
      if (ord.duration_days == 0) {
        q.erase(q.begin());
      }
      // If -1, we stay here forever (until order cancelled).
      continue;
    }

    if (!is_attack && !is_jump && !is_cargo_op && !is_body && !is_orbit && !is_scrap && dist <= arrive_eps) {
      q.erase(q.begin());
      continue;
    }

    auto transit_jump = [&]() {
      const Id jump_id = std::get<TravelViaJump>(q.front()).jump_point_id;
      const auto* jp = find_ptr(state_.jump_points, jump_id);
      if (!jp || jp->system_id != ship.system_id || jp->linked_jump_id == kInvalidId) return;

      const auto* dest = find_ptr(state_.jump_points, jp->linked_jump_id);
      if (!dest) return;

      const Id old_sys = ship.system_id;
      const Id new_sys = dest->system_id;

      if (auto* sys_old = find_ptr(state_.systems, old_sys)) {
        sys_old->ships.erase(std::remove(sys_old->ships.begin(), sys_old->ships.end(), ship_id), sys_old->ships.end());
      }

      ship.system_id = new_sys;
      ship.position_mkm = dest->position_mkm;

      if (auto* sys_new = find_ptr(state_.systems, new_sys)) {
        sys_new->ships.push_back(ship_id);
      }

      discover_system_for_faction(ship.faction_id, new_sys);

      {
        const auto* sys_new = find_ptr(state_.systems, new_sys);
        const std::string dest_name = sys_new ? sys_new->name : std::string("(unknown)");
        const std::string msg = "Ship " + ship.name + " transited jump point " + jp->name + " -> " + dest_name;
        nebula4x::log::info(msg);
        EventContext ctx;
        ctx.faction_id = ship.faction_id;
        ctx.system_id = new_sys;
        ctx.ship_id = ship_id;
        push_event(EventLevel::Info, EventCategory::Movement, msg, ctx);
      }
    };

    if (is_jump && dist <= dock_range) {
      ship.position_mkm = target;
      if (!is_coordinated_jump_group || allow_jump_transit) {
        transit_jump();
        q.erase(q.begin());
      }
      continue;
    }

    if (is_attack) {
      if (attack_has_contact) {
        if (dist <= desired_range) {
          continue;
        }
      } else {
        if (dist <= arrive_eps) {
          q.erase(q.begin());
          continue;
        }
      }
    }

    const auto* sd = find_design(ship.design_id);

    // Power gating: if engines draw power and the ship can't allocate it, it
    // cannot move this tick.
    double effective_speed_km_s = ship.speed_km_s;
    if (sd) {
      const auto p = compute_power_allocation(*sd);
      if (!p.engines_online && sd->power_use_engines > 1e-9) {
        effective_speed_km_s = 0.0;
      }
    }

    // Fleet speed matching: for ships in the same fleet with the same current
    // movement order, cap speed to the slowest ship in that cohort.
    if (cfg_.fleet_speed_matching && fleet_id != kInvalidId && !cohort_min_speed_km_s.empty()) {
      const auto key_opt = make_cohort_key(fleet_id, ship.system_id, q.front());
      if (key_opt) {
        const auto it_min = cohort_min_speed_km_s.find(*key_opt);
        if (it_min != cohort_min_speed_km_s.end()) {
          effective_speed_km_s = std::min(effective_speed_km_s, it_min->second);
        }
      }
    }

    const double max_step = mkm_per_day_from_speed(effective_speed_km_s, cfg_.seconds_per_day);
    if (max_step <= 0.0) continue;

    double step = max_step;
    if (is_attack) {
      step = std::min(step, std::max(0.0, dist - desired_range));
      if (step <= 0.0) continue;
    }

    const double fuel_cap = sd ? std::max(0.0, sd->fuel_capacity_tons) : 0.0;
    const double fuel_use = sd ? std::max(0.0, sd->fuel_use_per_mkm) : 0.0;
    const bool uses_fuel = (fuel_use > 0.0);
    if (uses_fuel) {
      // Be defensive for older saves/custom content that may not have been initialized yet.
      if (ship.fuel_tons < 0.0) ship.fuel_tons = fuel_cap;
      ship.fuel_tons = std::clamp(ship.fuel_tons, 0.0, fuel_cap);

      const double max_by_fuel = ship.fuel_tons / fuel_use;
      step = std::min(step, max_by_fuel);
      if (step <= 1e-12) continue;
    }

    auto burn_fuel = [&](double moved_mkm) {
      if (!uses_fuel || moved_mkm <= 0.0) return;
      const double before = ship.fuel_tons;
      const double burn = moved_mkm * fuel_use;
      ship.fuel_tons = std::max(0.0, ship.fuel_tons - burn);
      if (before > 1e-9 && ship.fuel_tons <= 1e-9) {
        const auto* sys = find_ptr(state_.systems, ship.system_id);
        const std::string sys_name = sys ? sys->name : std::string("(unknown)");
        const std::string msg = "Ship " + ship.name + " has run out of Fuel in " + sys_name;
        EventContext ctx;
        ctx.faction_id = ship.faction_id;
        ctx.system_id = ship.system_id;
        ctx.ship_id = ship_id;
        push_event(EventLevel::Warn, EventCategory::Movement, msg, ctx);
      }
    };

    if (dist <= step) {
      ship.position_mkm = target;

      burn_fuel(dist);

      if (is_jump) {
        if (!is_coordinated_jump_group || allow_jump_transit) {
          transit_jump();
          q.erase(q.begin());
        }
      } else if (is_attack) {
        if (!attack_has_contact) q.erase(q.begin());
      } else if (is_cargo_op) {
        const double moved = do_cargo_transfer();
        if (cargo_order_complete(moved)) q.erase(q.begin());
      } else if (is_scrap) {
          // Re-check scrap logic in case we arrived exactly on this frame
          // For now, simpler to wait for next tick's "in range" check which is cleaner
      } else if (is_orbit) {
          // Arrived at orbit body.
          // Don't pop; handled by duration logic next tick.
      } else {
        q.erase(q.begin());
      }
      continue;
    }

    const Vec2 dir = delta.normalized();
    ship.position_mkm += dir * step;
    burn_fuel(step);
  }
}


void Simulation::tick_combat() {
  std::unordered_map<Id, double> incoming_damage;
  std::unordered_map<Id, std::vector<Id>> attackers_for_target;

  auto is_hostile = [&](const Ship& a, const Ship& b) { return are_factions_hostile(a.faction_id, b.faction_id); };

  const auto ship_ids = sorted_keys(state_.ships);

  for (Id aid : ship_ids) {
    const auto* attacker_ptr = find_ptr(state_.ships, aid);
    if (!attacker_ptr) continue;
    const auto& attacker = *attacker_ptr;
    const auto* ad = find_design(attacker.design_id);
    if (!ad) continue;
    if (ad->weapon_damage <= 0.0 || ad->weapon_range_mkm <= 0.0) continue;

    // Power gating: if weapons draw power and the ship can't allocate it,
    // it cannot fire.
    {
      const auto p = compute_power_allocation(*ad);
      if (!p.weapons_online && ad->power_use_weapons > 1e-9) {
        continue;
      }
    }

    Id chosen = kInvalidId;
    double chosen_dist = 1e300;

    auto oit = state_.ship_orders.find(aid);
    if (oit != state_.ship_orders.end() && !oit->second.queue.empty() &&
        std::holds_alternative<AttackShip>(oit->second.queue.front())) {
      const Id tid = std::get<AttackShip>(oit->second.queue.front()).target_ship_id;
      const auto* tgt = find_ptr(state_.ships, tid);
      if (tgt && tgt->system_id == attacker.system_id && is_hostile(attacker, *tgt) &&
          is_ship_detected_by_faction(attacker.faction_id, tid)) {
        const double dist = (tgt->position_mkm - attacker.position_mkm).length();
        if (dist <= ad->weapon_range_mkm) {
          chosen = tid;
          chosen_dist = dist;
        }
      }
    }

    if (chosen == kInvalidId) {
      for (Id bid : ship_ids) {
        if (bid == aid) continue;
        const auto* target_ptr = find_ptr(state_.ships, bid);
        if (!target_ptr) continue;
        const auto& target = *target_ptr;

        if (target.system_id != attacker.system_id) continue;
        if (!is_hostile(attacker, target)) continue;
        if (!is_ship_detected_by_faction(attacker.faction_id, bid)) continue;

        const double dist = (target.position_mkm - attacker.position_mkm).length();
        if (dist > ad->weapon_range_mkm) continue;
        if (dist < chosen_dist) {
          chosen = bid;
          chosen_dist = dist;
        }
      }
    }

    if (chosen != kInvalidId) {
      incoming_damage[chosen] += ad->weapon_damage;
      attackers_for_target[chosen].push_back(aid);
    }
  }

  if (incoming_damage.empty()) return;

  auto fmt1 = [](double x) {
    std::ostringstream ss;
    ss.setf(std::ios::fixed);
    ss << std::setprecision(1) << x;
    return ss.str();
  };

  std::vector<Id> destroyed;
  destroyed.reserve(incoming_damage.size());

  // Track how much damage was absorbed by shields vs applied to hull.
  std::unordered_map<Id, double> shield_damage;
  std::unordered_map<Id, double> hull_damage;
  std::unordered_map<Id, double> pre_hp;
  std::unordered_map<Id, double> pre_shields;
  shield_damage.reserve(incoming_damage.size());
  hull_damage.reserve(incoming_damage.size());
  pre_hp.reserve(incoming_damage.size());
  pre_shields.reserve(incoming_damage.size());

  for (Id tid : sorted_keys(incoming_damage)) {
    const double dmg = incoming_damage[tid];
    auto* tgt = find_ptr(state_.ships, tid);
    if (!tgt) continue;

    pre_hp[tid] = tgt->hp;
    pre_shields[tid] = std::max(0.0, tgt->shields);

    double remaining = dmg;
    double absorbed = 0.0;
    if (tgt->shields > 0.0 && remaining > 0.0) {
      absorbed = std::min(tgt->shields, remaining);
      tgt->shields -= absorbed;
      remaining -= absorbed;
    }
    if (tgt->shields < 0.0) tgt->shields = 0.0;

    shield_damage[tid] = absorbed;
    hull_damage[tid] = remaining;

    tgt->hp -= remaining;
    if (tgt->hp <= 0.0) destroyed.push_back(tid);
  }

  // Damage events for ships that survive.
  // Destruction is logged separately below.
  {
    const double min_abs = std::max(0.0, cfg_.combat_damage_event_min_abs);
    const double min_frac = std::max(0.0, cfg_.combat_damage_event_min_fraction);
    const double warn_frac = std::clamp(cfg_.combat_damage_event_warn_remaining_fraction, 0.0, 1.0);

    for (Id tid : sorted_keys(incoming_damage)) {
      if (incoming_damage[tid] <= 1e-12) continue;

      const auto* tgt = find_ptr(state_.ships, tid);
      if (!tgt) continue;
      if (tgt->hp <= 0.0) continue; // handled by destruction log

      const double sh_dmg = (shield_damage.find(tid) != shield_damage.end()) ? shield_damage.at(tid) : 0.0;
      const double hull_dmg = (hull_damage.find(tid) != hull_damage.end()) ? hull_damage.at(tid) : 0.0;
      if (sh_dmg <= 1e-12 && hull_dmg <= 1e-12) continue;

      const auto* sys = find_ptr(state_.systems, tgt->system_id);
      const std::string sys_name = sys ? sys->name : std::string("(unknown)");

      // Use design max stats when available; otherwise approximate from pre-damage values.
      double max_hp = std::max(1.0, pre_hp.find(tid) != pre_hp.end() ? pre_hp.at(tid) : tgt->hp);
      double max_sh = std::max(0.0, pre_shields.find(tid) != pre_shields.end() ? pre_shields.at(tid) : 0.0);
      if (const auto* d = find_design(tgt->design_id)) {
        if (d->max_hp > 1e-9) max_hp = d->max_hp;
        max_sh = std::max(0.0, d->max_shields);
      }

      // Threshold on either hull damage or (if no hull damage) shield damage.
      double abs_metric = 0.0;
      double frac_metric = 0.0;
      if (hull_dmg > 1e-12) {
        abs_metric = hull_dmg;
        frac_metric = hull_dmg / std::max(1e-9, max_hp);
      } else {
        abs_metric = sh_dmg;
        frac_metric = (max_sh > 1e-9) ? (sh_dmg / std::max(1e-9, max_sh)) : 1.0;
      }
      if (abs_metric + 1e-12 < min_abs && frac_metric + 1e-12 < min_frac) continue;

      // Summarize attackers for context.
      Id attacker_ship_id = kInvalidId;
      Id attacker_fid = kInvalidId;
      std::string attacker_ship_name;
      std::string attacker_fac_name;
      std::size_t attackers_count = 0;

      if (auto ita = attackers_for_target.find(tid); ita != attackers_for_target.end()) {
        auto& vec = ita->second;
        std::sort(vec.begin(), vec.end());
        vec.erase(std::unique(vec.begin(), vec.end()), vec.end());
        attackers_count = vec.size();
        if (!vec.empty()) {
          attacker_ship_id = vec.front();
          if (const auto* atk = find_ptr(state_.ships, attacker_ship_id)) {
            attacker_fid = atk->faction_id;
            attacker_ship_name = atk->name;
            if (const auto* af = find_ptr(state_.factions, attacker_fid)) attacker_fac_name = af->name;
          }
        }
      }

      EventContext ctx;
      ctx.faction_id = tgt->faction_id;
      ctx.faction_id2 = attacker_fid;
      ctx.system_id = tgt->system_id;
      ctx.ship_id = tid;

      std::string msg;
      if (hull_dmg > 1e-12) {
        msg = "Ship damaged: " + tgt->name;
        msg += " took " + fmt1(hull_dmg) + " hull";
        if (sh_dmg > 1e-12) msg += " + " + fmt1(sh_dmg) + " shield";
        msg += " dmg";
      } else {
        msg = "Shields hit: " + tgt->name;
        msg += " took " + fmt1(sh_dmg) + " dmg";
      }

      msg += " (";
      if (max_sh > 1e-9) {
        msg += "Shields " + fmt1(std::max(0.0, tgt->shields)) + "/" + fmt1(max_sh) + ", ";
      }
      msg += "HP " + fmt1(std::max(0.0, tgt->hp)) + "/" + fmt1(max_hp) + ")";
      msg += " in " + sys_name;

      if (attacker_ship_id != kInvalidId) {
        msg += " (attacked by " + (attacker_ship_name.empty() ? (std::string("Ship ") + std::to_string(attacker_ship_id))
                                                          : attacker_ship_name);
        if (!attacker_fac_name.empty()) msg += " / " + attacker_fac_name;
        if (attackers_count > 1) msg += " +" + std::to_string(attackers_count - 1) + " more";
        msg += ")";
      }

      const double hp_frac = std::clamp(tgt->hp / std::max(1e-9, max_hp), 0.0, 1.0);
      double sh_frac = 1.0;
      if (max_sh > 1e-9) {
        sh_frac = std::clamp(tgt->shields / std::max(1e-9, max_sh), 0.0, 1.0);
      }
      const double remaining_frac = std::min(hp_frac, sh_frac);
      const EventLevel lvl = (remaining_frac <= warn_frac) ? EventLevel::Warn : EventLevel::Info;
      push_event(lvl, EventCategory::Combat, msg, ctx);
    }
  }

  std::sort(destroyed.begin(), destroyed.end());

  struct DestructionEvent {
    std::string msg;
    EventContext ctx;
  };
  std::vector<DestructionEvent> death_events;
  death_events.reserve(destroyed.size());

  for (Id dead_id : destroyed) {
    const auto it = state_.ships.find(dead_id);
    if (it == state_.ships.end()) continue;

    const Ship& victim = it->second;
    const Id sys_id = victim.system_id;
    const Id victim_fid = victim.faction_id;

    const auto* sys = find_ptr(state_.systems, sys_id);
    const std::string sys_name = sys ? sys->name : std::string("(unknown)");

    const auto* victim_fac = find_ptr(state_.factions, victim_fid);
    const std::string victim_fac_name = victim_fac ? victim_fac->name : std::string("(unknown)");

    Id attacker_ship_id = kInvalidId;
    Id attacker_fid = kInvalidId;
    std::string attacker_ship_name;
    std::string attacker_fac_name;
    std::size_t attackers_count = 0;

    if (auto ita = attackers_for_target.find(dead_id); ita != attackers_for_target.end()) {
      auto& vec = ita->second;
      std::sort(vec.begin(), vec.end());
      vec.erase(std::unique(vec.begin(), vec.end()), vec.end());
      attackers_count = vec.size();
      if (!vec.empty()) {
        attacker_ship_id = vec.front();
        if (const auto* atk = find_ptr(state_.ships, attacker_ship_id)) {
          attacker_fid = atk->faction_id;
          attacker_ship_name = atk->name;
          if (const auto* af = find_ptr(state_.factions, attacker_fid)) attacker_fac_name = af->name;
        }
      }
    }

    EventContext ctx;
    ctx.faction_id = victim_fid;
    ctx.faction_id2 = attacker_fid;
    ctx.system_id = sys_id;
    ctx.ship_id = dead_id;

    std::string msg = "Ship destroyed: " + victim.name;
    msg += " (" + victim_fac_name + ")";
    msg += " in " + sys_name;

    if (attacker_ship_id != kInvalidId) {
      msg += " (killed by " + (attacker_ship_name.empty() ? std::string("Ship ") + std::to_string(attacker_ship_id)
                                                         : attacker_ship_name);
      if (!attacker_fac_name.empty()) msg += " / " + attacker_fac_name;
      if (attackers_count > 1) msg += " +" + std::to_string(attackers_count - 1) + " more";
      msg += ")";
    }

    death_events.push_back(DestructionEvent{std::move(msg), ctx});
  }

  for (Id dead_id : destroyed) {
    auto it = state_.ships.find(dead_id);
    if (it == state_.ships.end()) continue;

    const Id sys_id = it->second.system_id;

    if (auto* sys = find_ptr(state_.systems, sys_id)) {
      sys->ships.erase(std::remove(sys->ships.begin(), sys->ships.end(), dead_id), sys->ships.end());
    }

    state_.ship_orders.erase(dead_id);
    state_.ships.erase(dead_id);

    // Keep fleet membership consistent.
    remove_ship_from_fleets(dead_id);

    for (auto& [_, fac] : state_.factions) {
      fac.ship_contacts.erase(dead_id);
    }
  }

  for (const auto& e : death_events) {
    nebula4x::log::warn(e.msg);
    push_event(EventLevel::Warn, EventCategory::Combat, e.msg, e.ctx);
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
