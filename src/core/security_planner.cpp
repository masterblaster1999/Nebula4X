#include "nebula4x/core/security_planner.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_set>
#include <utility>

#include "nebula4x/core/simulation.h"
#include "nebula4x/util/sorted_keys.h"

namespace nebula4x {

namespace {

using util::sorted_keys;

struct EdgeKey {
  Id a{kInvalidId};
  Id b{kInvalidId};

  static EdgeKey make(Id x, Id y) {
    if (x < y) return {x, y};
    return {y, x};
  }

  bool operator==(const EdgeKey& o) const { return a == o.a && b == o.b; }
};

struct EdgeKeyHash {
  std::size_t operator()(const EdgeKey& k) const noexcept {
    std::size_t h = 0xcbf29ce484222325ULL;
    auto mix = [&](std::uint64_t v) {
      h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    };
    mix(static_cast<std::uint64_t>(k.a));
    mix(static_cast<std::uint64_t>(k.b));
    return h;
  }
};

double suppression_to_power(double suppression, double scale) {
  suppression = std::clamp(suppression, 0.0, 0.999999);
  scale = std::max(1e-6, scale);
  return -scale * std::log(std::max(1e-12, 1.0 - suppression));
}

// Compute a 0..1 risk term used for trade convoy weighting.
double trade_endpoint_risk(const Simulation& sim,
                           Id system_id,
                           const std::unordered_map<Id, double>& blockade_pressure,
                           const std::unordered_map<Id, double>& shipping_loss_pressure) {
  const auto& cfg = sim.cfg();
  const double piracy = std::clamp(sim.piracy_risk_for_system(system_id), 0.0, 1.0);

  double blockade = 0.0;
  if (auto it = blockade_pressure.find(system_id); it != blockade_pressure.end()) blockade = it->second;

  double ship_loss = 0.0;
  if (auto it = shipping_loss_pressure.find(system_id); it != shipping_loss_pressure.end()) ship_loss = it->second;

  const double bw = std::max(0.0, cfg.civilian_trade_convoy_blockade_risk_weight);
  const double sw = std::max(0.0, cfg.civilian_trade_convoy_shipping_loss_risk_weight);

  const double combined = piracy + bw * blockade + sw * ship_loss;
  return std::clamp(combined, 0.0, 1.0);
}

std::unordered_map<Id, Vec2> compute_trade_hub_positions(const GameState& st) {
  // Choose one body position per system (largest population colony).
  std::unordered_map<Id, Vec2> hub_pos;
  std::unordered_map<Id, double> hub_pop;
  hub_pos.reserve(st.colonies.size() + 8);
  hub_pop.reserve(st.colonies.size() + 8);

  for (const auto& [cid, c] : st.colonies) {
    (void)cid;
    const Body* b = find_ptr(st.bodies, c.body_id);
    if (!b) continue;
    if (b->system_id == kInvalidId) continue;
    const double pop = std::max(0.0, c.population_millions);
    auto itp = hub_pop.find(b->system_id);
    if (itp == hub_pop.end() || pop > itp->second + 1e-9) {
      hub_pop[b->system_id] = pop;
      hub_pos[b->system_id] = b->position_mkm;
    }
  }

  return hub_pos;
}

std::unordered_map<Id, double> compute_blockade_pressure_by_system(const Simulation& sim) {
  std::unordered_map<Id, double> out;
  const auto& st = sim.state();
  out.reserve(st.systems.size() * 2 + 8);
  for (const auto& [cid, c] : st.colonies) {
    (void)cid;
    const Body* b = find_ptr(st.bodies, c.body_id);
    if (!b) continue;
    if (b->system_id == kInvalidId) continue;
    const double p = std::clamp(sim.blockade_status_for_colony(c.id).pressure, 0.0, 1.0);
    auto it = out.find(b->system_id);
    if (it == out.end() || p > it->second + 1e-12) {
      out[b->system_id] = p;
    }
  }
  return out;
}

std::unordered_map<Id, double> compute_shipping_loss_pressure_by_system(const Simulation& sim) {
  std::unordered_map<Id, double> out;
  const auto& st = sim.state();
  out.reserve(st.systems.size() * 2 + 8);
  for (Id sys_id : sorted_keys(st.systems)) {
    const double p = std::clamp(sim.civilian_shipping_loss_pressure_for_system(sys_id), 0.0, 1.0);
    if (p > 1e-12) out[sys_id] = p;
  }
  return out;
}

std::pair<Id, Id> resolve_jump_pair(const GameState& st, Id sys_a, Id sys_b) {
  if (sys_a == kInvalidId || sys_b == kInvalidId) return {kInvalidId, kInvalidId};
  const StarSystem* a = find_ptr(st.systems, sys_a);
  const StarSystem* b = find_ptr(st.systems, sys_b);
  if (!a || !b) return {kInvalidId, kInvalidId};

  Id jp_a = kInvalidId;
  Id jp_b = kInvalidId;

  for (Id jpid : a->jump_points) {
    const JumpPoint* jp = find_ptr(st.jump_points, jpid);
    if (!jp) continue;
    const JumpPoint* linked = (jp->linked_jump_id != kInvalidId) ? find_ptr(st.jump_points, jp->linked_jump_id) : nullptr;
    if (!linked) continue;
    if (linked->system_id == sys_b) {
      jp_a = jp->id;
      jp_b = linked->id;
      break;
    }
  }

  // Fallback: scan B if not found by following A links.
  if (jp_a == kInvalidId) {
    for (Id jpid : b->jump_points) {
      const JumpPoint* jp = find_ptr(st.jump_points, jpid);
      if (!jp) continue;
      const JumpPoint* linked = (jp->linked_jump_id != kInvalidId) ? find_ptr(st.jump_points, jp->linked_jump_id) : nullptr;
      if (!linked) continue;
      if (linked->system_id == sys_a) {
        jp_b = jp->id;
        jp_a = linked->id;
        break;
      }
    }
  }

  return {jp_a, jp_b};
}

}  // namespace

SecurityPlannerResult compute_security_plan(const Simulation& sim, const SecurityPlannerOptions& opt) {
  SecurityPlannerResult res;
  const auto& st = sim.state();

  const Id fid = opt.faction_id;
  const Faction* fac = (fid != kInvalidId) ? find_ptr(st.factions, fid) : nullptr;
  if (fid != kInvalidId && !fac) {
    res.ok = false;
    res.message = "Invalid faction id";
    return res;
  }

  // Collect discovered systems for fog-of-war filtering.
  std::unordered_set<Id> discovered;
  if (opt.restrict_to_discovered && fac) {
    discovered.reserve(fac->discovered_systems.size() * 2 + 8);
    for (Id sid : fac->discovered_systems) discovered.insert(sid);
  }

  // Systems containing our colonies represent direct economic exposure.
  std::unordered_set<Id> own_colony_systems;
  if (fid != kInvalidId) {
    own_colony_systems.reserve(st.colonies.size() + 8);
    for (const auto& [cid, c] : st.colonies) {
      (void)cid;
      if (c.faction_id != fid) continue;
      const Body* b = find_ptr(st.bodies, c.body_id);
      if (!b) continue;
      if (b->system_id == kInvalidId) continue;
      own_colony_systems.insert(b->system_id);
    }
  }

  // Precompute hub positions and risk components.
  const auto hub_pos = compute_trade_hub_positions(st);
  const auto blockade_by_system = compute_blockade_pressure_by_system(sim);
  const auto shipping_loss_by_system = compute_shipping_loss_pressure_by_system(sim);

  TradeNetworkOptions topt;
  topt.max_lanes = std::max(1, opt.max_lanes);
  topt.include_uncolonized_markets = false;
  topt.include_colony_contributions = true;
  TradeNetwork net = compute_trade_network(sim, topt);

  // Score systems by trade throughput, amplified by endpoint risk.
  std::unordered_map<Id, SecuritySystemNeed> sys;
  sys.reserve(64);

  // Accumulate chokepoint edge traffic.
  struct EdgeAccum {
    double traffic{0.0};
    double risk_weighted_sum{0.0};
    double max_risk{0.0};
  };
  std::unordered_map<EdgeKey, EdgeAccum, EdgeKeyHash> edges;
  edges.reserve(64);

  std::vector<SecurityCorridor> corridors;
  corridors.reserve(static_cast<std::size_t>(topt.max_lanes));

  int considered = 0;
  int skipped_unreachable = 0;
  int skipped_fow = 0;
  int skipped_exposure = 0;

  const double min_lane_vol = std::max(0.0, opt.min_lane_volume);
  const double risk_w = std::max(0.0, opt.risk_weight);
  const double own_w = std::max(1.0, opt.own_colony_weight);

  for (const auto& lane : net.lanes) {
    if (!(lane.total_volume > min_lane_vol)) continue;
    if (lane.from_system_id == kInvalidId || lane.to_system_id == kInvalidId) continue;
    if (lane.from_system_id == lane.to_system_id) continue;

    if (opt.require_own_colony_endpoints && fid != kInvalidId) {
      const bool relevant = own_colony_systems.contains(lane.from_system_id) || own_colony_systems.contains(lane.to_system_id);
      if (!relevant) {
        ++skipped_exposure;
        continue;
      }
    }

    if (opt.restrict_to_discovered && fac) {
      if (!discovered.contains(lane.from_system_id) || !discovered.contains(lane.to_system_id)) {
        ++skipped_fow;
        continue;
      }
    }

    Vec2 start_pos_mkm{0.0, 0.0};
    if (auto it = hub_pos.find(lane.from_system_id); it != hub_pos.end()) start_pos_mkm = it->second;
    std::optional<Vec2> goal_pos_mkm;
    if (auto it = hub_pos.find(lane.to_system_id); it != hub_pos.end()) goal_pos_mkm = it->second;

    const auto plan = sim.plan_jump_route_from_pos(lane.from_system_id, start_pos_mkm, fid,
                                                   opt.planning_speed_km_s, lane.to_system_id,
                                                   opt.restrict_to_discovered, goal_pos_mkm);
    if (!plan || plan->systems.empty()) {
      ++skipped_unreachable;
      continue;
    }

    ++considered;

    const double vol_share = lane.total_volume / static_cast<double>(plan->systems.size());

    SecurityCorridor corr;
    corr.from_system_id = lane.from_system_id;
    corr.to_system_id = lane.to_system_id;
    corr.volume = lane.total_volume;
    corr.route_systems = plan->systems;
    corr.top_flows = lane.top_flows;

    double risk_sum = 0.0;
    double max_r = 0.0;
    for (Id sys_id : plan->systems) {
      if (sys_id == kInvalidId) continue;
      const StarSystem* ss = find_ptr(st.systems, sys_id);
      const Id rid = ss ? ss->region_id : kInvalidId;

      SecuritySystemNeed& e = sys[sys_id];
      e.system_id = sys_id;
      e.region_id = rid;
      e.trade_throughput += vol_share;

      e.piracy_risk = std::max(e.piracy_risk, std::clamp(sim.piracy_risk_for_system(sys_id), 0.0, 1.0));
      if (auto it = blockade_by_system.find(sys_id); it != blockade_by_system.end()) {
        e.blockade_pressure = std::max(e.blockade_pressure, it->second);
      }
      if (auto it = shipping_loss_by_system.find(sys_id); it != shipping_loss_by_system.end()) {
        e.shipping_loss_pressure = std::max(e.shipping_loss_pressure, it->second);
      }
      e.endpoint_risk = std::max(e.endpoint_risk, trade_endpoint_risk(sim, sys_id, blockade_by_system, shipping_loss_by_system));
      e.has_own_colony = (fid != kInvalidId) ? own_colony_systems.contains(sys_id) : false;

      double need = vol_share * (0.20 + risk_w * e.endpoint_risk);
      if (e.has_own_colony) need *= own_w;
      e.need += need;

      risk_sum += e.endpoint_risk;
      max_r = std::max(max_r, e.endpoint_risk);
    }

    corr.max_risk = max_r;
    corr.avg_risk = plan->systems.empty() ? 0.0 : (risk_sum / static_cast<double>(plan->systems.size()));

    // Accumulate edge traffic for chokepoints.
    for (std::size_t i = 1; i < plan->systems.size(); ++i) {
      const Id a = plan->systems[i - 1];
      const Id b = plan->systems[i];
      if (a == kInvalidId || b == kInvalidId) continue;
      if (a == b) continue;
      const double ra = trade_endpoint_risk(sim, a, blockade_by_system, shipping_loss_by_system);
      const double rb = trade_endpoint_risk(sim, b, blockade_by_system, shipping_loss_by_system);
      const double r = 0.5 * (ra + rb);

      EdgeAccum& acc = edges[EdgeKey::make(a, b)];
      acc.traffic += lane.total_volume;
      acc.risk_weighted_sum += r * lane.total_volume;
      acc.max_risk = std::max(acc.max_risk, r);
    }

    corridors.push_back(std::move(corr));
  }

  if (considered == 0) {
    res.ok = true;
    res.message = "No eligible trade lanes";
    return res;
  }

  // Convert system needs into sorted rows.
  std::vector<SecuritySystemNeed> sys_rows;
  sys_rows.reserve(sys.size());
  for (auto& [sid, e] : sys) {
    (void)sid;
    sys_rows.push_back(e);
  }
  std::sort(sys_rows.begin(), sys_rows.end(), [](const auto& a, const auto& b) {
    if (a.need > b.need + 1e-9) return true;
    if (b.need > a.need + 1e-9) return false;
    return a.system_id < b.system_id;
  });

  // Aggregate region needs.
  std::unordered_map<Id, SecurityRegionNeed> reg;
  reg.reserve(32);
  for (const auto& e : sys_rows) {
    if (e.region_id == kInvalidId) continue;
    SecurityRegionNeed& r = reg[e.region_id];
    r.region_id = e.region_id;
    r.need += e.need;
    if (r.representative_system_id == kInvalidId || e.need > r.representative_system_need + 1e-9 ||
        (std::abs(e.need - r.representative_system_need) <= 1e-9 && e.system_id < r.representative_system_id)) {
      r.representative_system_id = e.system_id;
      r.representative_system_need = e.need;
    }
  }

  const double scale = std::max(1e-6, sim.cfg().pirate_suppression_power_scale);
  std::vector<SecurityRegionNeed> reg_rows;
  reg_rows.reserve(reg.size());
  for (auto& [rid, r] : reg) {
    (void)rid;
    const Region* rr = find_ptr(st.regions, r.region_id);
    if (rr) {
      r.pirate_risk = std::clamp(rr->pirate_risk, 0.0, 1.0);
      r.pirate_suppression = std::clamp(rr->pirate_suppression, 0.0, 1.0);
    }
    r.effective_piracy_risk = std::clamp(r.pirate_risk * (1.0 - r.pirate_suppression), 0.0, 1.0);
    r.implied_patrol_power = suppression_to_power(r.pirate_suppression, scale);
    const double desired_sup = std::clamp(opt.desired_region_suppression, 0.0, 0.999999);
    r.desired_patrol_power = suppression_to_power(desired_sup, scale);
    r.additional_patrol_power = std::max(0.0, r.desired_patrol_power - r.implied_patrol_power);
    reg_rows.push_back(r);
  }
  std::sort(reg_rows.begin(), reg_rows.end(), [](const auto& a, const auto& b) {
    if (a.need > b.need + 1e-9) return true;
    if (b.need > a.need + 1e-9) return false;
    return a.region_id < b.region_id;
  });

  // Sort corridors by volume then risk.
  std::sort(corridors.begin(), corridors.end(), [](const auto& a, const auto& b) {
    if (a.volume > b.volume + 1e-9) return true;
    if (b.volume > a.volume + 1e-9) return false;
    if (a.max_risk > b.max_risk + 1e-9) return true;
    if (b.max_risk > a.max_risk + 1e-9) return false;
    if (a.from_system_id != b.from_system_id) return a.from_system_id < b.from_system_id;
    return a.to_system_id < b.to_system_id;
  });

  // Build chokepoint list.
  std::vector<SecurityChokepoint> chok;
  chok.reserve(edges.size());
  for (const auto& [k, acc] : edges) {
    if (!(acc.traffic > min_lane_vol)) continue;
    SecurityChokepoint c;
    c.system_a_id = k.a;
    c.system_b_id = k.b;
    c.traffic = acc.traffic;
    c.avg_risk = (acc.traffic > 1e-9) ? std::clamp(acc.risk_weighted_sum / acc.traffic, 0.0, 1.0) : 0.0;
    c.max_risk = std::clamp(acc.max_risk, 0.0, 1.0);
    const auto jp = resolve_jump_pair(st, k.a, k.b);
    c.jump_a_to_b = jp.first;
    c.jump_b_to_a = jp.second;
    chok.push_back(c);
  }
  std::sort(chok.begin(), chok.end(), [](const auto& a, const auto& b) {
    if (a.traffic > b.traffic + 1e-9) return true;
    if (b.traffic > a.traffic + 1e-9) return false;
    if (a.max_risk > b.max_risk + 1e-9) return true;
    if (b.max_risk > a.max_risk + 1e-9) return false;
    if (a.system_a_id != b.system_a_id) return a.system_a_id < b.system_a_id;
    return a.system_b_id < b.system_b_id;
  });

  const int cap = std::max(1, opt.max_results);
  auto take_cap = [&](auto& v) {
    if ((int)v.size() > cap) {
      v.resize(static_cast<std::size_t>(cap));
      res.truncated = true;
    }
  };

  res.ok = true;
  res.message = "ok";
  res.top_regions = std::move(reg_rows);
  res.top_systems = std::move(sys_rows);
  res.top_corridors = std::move(corridors);
  res.top_chokepoints = std::move(chok);
  take_cap(res.top_regions);
  take_cap(res.top_systems);
  take_cap(res.top_corridors);
  take_cap(res.top_chokepoints);

  if (res.top_corridors.empty() && skipped_unreachable > 0) {
    res.message = "No reachable lanes under current fog-of-war constraints";
  } else if (res.top_corridors.empty()) {
    res.message = "No corridors";
  }

  return res;
}

}  // namespace nebula4x
