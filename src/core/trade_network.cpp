#include "nebula4x/core/trade_network.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "nebula4x/core/simulation.h"
#include "nebula4x/util/sorted_keys.h"
#include "nebula4x/util/hash_rng.h"

namespace nebula4x {
namespace {

double hash_unit01(std::uint64_t x) {
  const std::uint64_t h = ::nebula4x::util::splitmix64(x);
  return ::nebula4x::util::u01_from_u64(h);
}

double clamp01(double x) {
  if (x < 0.0) return 0.0;
  if (x > 1.0) return 1.0;
  return x;
}

double safe_log1p(double x) {
  if (!(x > -1.0)) return 0.0;
  return std::log1p(x);
}

TradeGoodKind kind_for_resource(const ContentDB& content, const std::string& resource_id) {
  if (resource_id == "Fuel") return TradeGoodKind::Fuel;
  if (resource_id == "Munitions") return TradeGoodKind::Munitions;
  if (resource_id == "Metals") return TradeGoodKind::ProcessedMetals;
  if (resource_id == "Minerals") return TradeGoodKind::ProcessedMinerals;

  const auto it = content.resources.find(resource_id);
  if (it == content.resources.end()) {
    // Unknown modded resource: fall back to a neutral-ish bucket.
    return TradeGoodKind::RawMinerals;
  }
  const ResourceDef& r = it->second;
  const std::string& cat = r.category;
  if (cat == "volatile") return TradeGoodKind::Volatiles;
  if (cat == "exotic") return TradeGoodKind::Exotics;
  if (cat == "metal") {
    // Mineable metals vs processed metals share the category tag; treat mineables
    // as raw inputs.
    return r.mineable ? TradeGoodKind::RawMetals : TradeGoodKind::ProcessedMetals;
  }
  if (cat == "fuel") return TradeGoodKind::Fuel;
  if (cat == "munitions") return TradeGoodKind::Munitions;
  // Default: non-volatile minerals.
  return r.mineable ? TradeGoodKind::RawMinerals : TradeGoodKind::ProcessedMinerals;
}

struct DepositTotals {
  double metal_tons{0.0};
  double mineral_tons{0.0};
  double volatile_tons{0.0};
  double exotic_tons{0.0};
};

DepositTotals compute_deposit_totals(const GameState& s, const ContentDB& content, const StarSystem& sys) {
  DepositTotals out;
  for (Id bid : sys.bodies) {
    const auto* b = find_ptr(s.bodies, bid);
    if (!b) continue;
    // Use sorted mineral keys for determinism across platforms.
    std::vector<std::string> keys;
    keys.reserve(b->mineral_deposits.size());
    for (const auto& kv : b->mineral_deposits) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());
    for (const auto& mineral : keys) {
      const double tons = std::max(0.0, b->mineral_deposits.at(mineral));
      if (tons <= 0.0) continue;
      const auto it = content.resources.find(mineral);
      const std::string cat = (it != content.resources.end()) ? it->second.category : std::string{};
      if (cat == "volatile") out.volatile_tons += tons;
      else if (cat == "exotic") out.exotic_tons += tons;
      else if (cat == "metal") out.metal_tons += tons;
      else out.mineral_tons += tons;
    }
  }
  return out;
}

double system_habitable_score(const StarSystem& sys, const GameState& s) {
  double best = 0.0;
  for (Id bid : sys.bodies) {
    const auto* b = find_ptr(s.bodies, bid);
    if (!b) continue;
    if (b->type != BodyType::Planet && b->type != BodyType::Moon) continue;

    double score = 0.0;
    if (b->terraforming_complete) {
      score = 1.0;
    } else {
      const double t = b->surface_temp_k;
      const double atm = b->atmosphere_atm;
      // Approximate surface gravity (in Earth-g) if the generator provided mass/radius.
      double g = 1.0;
      if (b->radius_km > 0.0 && b->mass_earths > 0.0) {
        const double r_earth = b->radius_km / 6371.0;
        if (r_earth > 0.0) g = b->mass_earths / (r_earth * r_earth);
      }
      // Very simple heuristic: closeness to Earthlike values.
      const double temp_ok = clamp01(1.0 - std::fabs(t - 288.0) / 120.0);
      const double atm_ok = clamp01(1.0 - std::fabs(atm - 1.0) / 1.5);
      const double grav_ok = clamp01(1.0 - std::fabs(g - 1.0) / 1.0);
      score = temp_ok * atm_ok * grav_ok;
    }
    if (score > best) best = score;
  }
  return best;
}

// Compute all-pairs shortest path distances over the jump network.
//
// Edge weights are galaxy-space Euclidean lengths between connected systems.
// The result is used as a travel-cost proxy for trade decay.
std::vector<std::vector<double>> jump_graph_distances(const GameState& s, const std::vector<Id>& system_ids) {
  const int n = static_cast<int>(system_ids.size());
  const double kInf = std::numeric_limits<double>::infinity();
  std::vector<std::vector<double>> dist(n, std::vector<double>(n, kInf));
  if (n == 0) return dist;

  std::unordered_map<Id, int> idx;
  idx.reserve(system_ids.size() * 2);
  for (int i = 0; i < n; ++i) idx[system_ids[i]] = i;

  std::vector<Vec2> pos(n);
  for (int i = 0; i < n; ++i) {
    const auto* sys = find_ptr(s.systems, system_ids[i]);
    pos[i] = sys ? sys->galaxy_pos : Vec2{0.0, 0.0};
  }

  std::vector<std::vector<std::pair<int, double>>> adj(n);
  for (Id jid : util::sorted_keys(s.jump_points)) {
    const auto& jp = s.jump_points.at(jid);
    const auto* other = find_ptr(s.jump_points, jp.linked_jump_id);
    if (!other) continue;
    const auto it_a = idx.find(jp.system_id);
    const auto it_b = idx.find(other->system_id);
    if (it_a == idx.end() || it_b == idx.end()) continue;
    const int a = it_a->second;
    const int b = it_b->second;
    if (a == b) continue;
    const double w = (pos[a] - pos[b]).length();
    adj[a].push_back({b, w});
  }

  // Ensure deterministic processing order in the shortest-path pass.
  for (auto& edges : adj) {
    std::sort(edges.begin(), edges.end(), [](const auto& lhs, const auto& rhs) {
      if (lhs.first != rhs.first) return lhs.first < rhs.first;
      return lhs.second < rhs.second;
    });
  }

  // Dijkstra from each node.
  for (int src = 0; src < n; ++src) {
    dist[src][src] = 0.0;
    using Item = std::pair<double, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
    pq.push({0.0, src});

    while (!pq.empty()) {
      const auto [d, u] = pq.top();
      pq.pop();
      if (d > dist[src][u] + 1e-12) continue;
      for (const auto& [v, w] : adj[u]) {
        const double nd = d + w;
        if (nd + 1e-12 < dist[src][v]) {
          dist[src][v] = nd;
          pq.push({nd, v});
        }
      }
    }
  }
  return dist;
}

}  // namespace

TradeNetwork compute_trade_network(const Simulation& sim, const TradeNetworkOptions& opt) {
  TradeNetwork out;

  const GameState& s = sim.state();
  const ContentDB& content = sim.content();

  const std::vector<Id> system_ids = util::sorted_keys(s.systems);
  const int n = static_cast<int>(system_ids.size());
  out.nodes.reserve(system_ids.size());

  if (n == 0) return out;

  std::unordered_map<Id, int> sys_index;
  sys_index.reserve(system_ids.size() * 2);
  for (int i = 0; i < n; ++i) sys_index[system_ids[i]] = i;

  // Compute some global normalizers for hub scoring.
  Vec2 center{0.0, 0.0};
  for (Id sid : system_ids) {
    const auto* sys = find_ptr(s.systems, sid);
    if (!sys) continue;
    center.x += sys->galaxy_pos.x;
    center.y += sys->galaxy_pos.y;
  }
  center.x /= static_cast<double>(system_ids.size());
  center.y /= static_cast<double>(system_ids.size());

  double max_radius = 1.0;
  int max_degree = 1;
  for (Id sid : system_ids) {
    const auto* sys = find_ptr(s.systems, sid);
    if (!sys) continue;
    max_radius = std::max(max_radius, (sys->galaxy_pos - center).length());
    max_degree = std::max(max_degree, static_cast<int>(sys->jump_points.size()));
  }


  // Aggregate colony contributions per system.
  std::vector<std::array<double, kTradeGoodKindCount>> colony_supply(n);
  std::vector<std::array<double, kTradeGoodKindCount>> colony_demand(n);
  std::vector<double> colony_pop_m(n, 0.0);

  if (opt.include_colony_contributions) {
    for (Id cid : util::sorted_keys(s.colonies)) {
      const Colony& c = s.colonies.at(cid);
      const auto* b = find_ptr(s.bodies, c.body_id);
      if (!b) continue;
      const auto it = sys_index.find(b->system_id);
      if (it == sys_index.end()) continue;
      const int si = it->second;

      const double pop = std::max(0.0, c.population_millions);
      colony_pop_m[si] += pop;

      // Baseline civilian demand signal.
      colony_demand[si][trade_good_index(TradeGoodKind::ProcessedMetals)] += pop / 50000.0;
      colony_demand[si][trade_good_index(TradeGoodKind::ProcessedMinerals)] += pop / 60000.0;
      colony_demand[si][trade_good_index(TradeGoodKind::Fuel)] += pop / 40000.0;

      // Deterministic iteration order: installations + resource maps are unordered_maps.
      for (const auto& inst_id : util::sorted_keys(c.installations)) {
        const int count = c.installations.at(inst_id);
        if (count <= 0) continue;
        const auto def_it = content.installations.find(inst_id);
        if (def_it == content.installations.end()) continue;
        const InstallationDef& def = def_it->second;

        for (const auto& res_id : util::sorted_keys(def.produces_per_day)) {
          const double per_day = def.produces_per_day.at(res_id);
          if (per_day <= 0.0) continue;
          const TradeGoodKind k = kind_for_resource(content, res_id);
          colony_supply[si][trade_good_index(k)] += (per_day * static_cast<double>(count)) / std::max(1.0, opt.colony_tons_per_unit);
        }
        for (const auto& res_id : util::sorted_keys(def.consumes_per_day)) {
          const double per_day = def.consumes_per_day.at(res_id);
          if (per_day <= 0.0) continue;
          const TradeGoodKind k = kind_for_resource(content, res_id);
          colony_demand[si][trade_good_index(k)] += (per_day * static_cast<double>(count)) / std::max(1.0, opt.colony_tons_per_unit);
        }
      }
    }
  }

  // Build per-system nodes.
  std::vector<std::array<double, kTradeGoodKindCount>> net_balance(n);
  std::vector<double> hub_score(n, 0.0);
  std::vector<double> market_size(n, 0.0);

  for (int i = 0; i < n; ++i) {
    const Id sid = system_ids[i];
    const auto* sys = find_ptr(s.systems, sid);
    if (!sys) continue;

    const auto* reg = (sys->region_id != kInvalidId) ? find_ptr(s.regions, sys->region_id) : nullptr;
    const double reg_mineral = reg ? std::max(0.0, reg->mineral_richness_mult) : 1.0;
    const double reg_volatile = reg ? std::max(0.0, reg->volatile_richness_mult) : 1.0;
    const double reg_pirate = reg ? clamp01(reg->pirate_risk) : 0.0;
    const double reg_ruins = reg ? clamp01(reg->ruins_density) : 0.0;

    const double radius01 = clamp01((sys->galaxy_pos - center).length() / max_radius);
    const double deg01 = clamp01(static_cast<double>(sys->jump_points.size()) / static_cast<double>(max_degree));
    const double hub = clamp01(0.6 * deg01 + 0.4 * (1.0 - radius01));
    hub_score[i] = hub;

    const DepositTotals dep = compute_deposit_totals(s, content, *sys);
    const double metal_f = clamp01(safe_log1p(dep.metal_tons / 1.0e6) / 6.0);
    const double mineral_f = clamp01(safe_log1p(dep.mineral_tons / 1.0e6) / 6.0);
    const double volatile_f = clamp01(safe_log1p(dep.volatile_tons / 1.0e6) / 6.0);
    const double exotic_f = clamp01(safe_log1p(dep.exotic_tons / 2.0e5) / 6.0);

    const double hab = system_habitable_score(*sys, s);
    const double mining_strength = clamp01(0.25 * metal_f + 0.25 * mineral_f + 0.25 * volatile_f + 0.25 * exotic_f);
    const double industry_strength = clamp01(0.6 * hab + 0.4 * hub);
    const double military_strength = clamp01(0.65 * reg_pirate + 0.35 * hub);
    const double research_strength = clamp01(0.8 * reg_ruins + 0.2 * (1.0 - clamp01(sys->nebula_density)));

    // Tiny deterministic perturbation to avoid excessive ties.
    const double noise = (hash_unit01(static_cast<std::uint64_t>(sid) ^ 0x5a7dULL) - 0.5) * 0.08;

    double m = 0.15 + 0.45 * hub + 0.35 * hab;
    if (!opt.include_uncolonized_markets) {
      m = 0.0;
    }
    if (opt.include_colony_contributions) {
      // Colonies add market mass even if uncolonized markets are disabled.
      m += colony_pop_m[i] / 20000.0;
    }
    m *= (1.0 + noise);
    m = std::max(0.0, m);
    market_size[i] = m;

    std::array<double, kTradeGoodKindCount> supply{};
    std::array<double, kTradeGoodKindCount> demand{};

    // Base supply (procedural).
    supply[trade_good_index(TradeGoodKind::RawMetals)] = m * (0.25 + 0.85 * metal_f) * reg_mineral;
    supply[trade_good_index(TradeGoodKind::RawMinerals)] = m * (0.25 + 0.85 * mineral_f) * reg_mineral;
    supply[trade_good_index(TradeGoodKind::Volatiles)] = m * (0.15 + 0.90 * volatile_f) * reg_volatile;
    supply[trade_good_index(TradeGoodKind::Exotics)] = m * (0.04 + 0.96 * exotic_f) * reg_mineral;

    supply[trade_good_index(TradeGoodKind::ProcessedMetals)] = m * (0.15 + 0.85 * industry_strength);
    supply[trade_good_index(TradeGoodKind::ProcessedMinerals)] = m * (0.12 + 0.80 * industry_strength);
    supply[trade_good_index(TradeGoodKind::Fuel)] = m * (0.10 + 0.55 * industry_strength + 0.25 * volatile_f);
    supply[trade_good_index(TradeGoodKind::Munitions)] = m * (0.05 + 0.35 * industry_strength + 0.45 * military_strength);

    // Base demand (procedural).
    demand[trade_good_index(TradeGoodKind::RawMetals)] = m * (0.10 + 0.90 * industry_strength);
    demand[trade_good_index(TradeGoodKind::RawMinerals)] = m * (0.10 + 0.80 * industry_strength);
    demand[trade_good_index(TradeGoodKind::Volatiles)] = m * (0.05 + 0.30 * industry_strength + 0.65 * military_strength);
    demand[trade_good_index(TradeGoodKind::Exotics)] = m * (0.03 + 0.55 * research_strength + 0.20 * industry_strength);

    demand[trade_good_index(TradeGoodKind::ProcessedMetals)] = m * (0.06 + 0.65 * mining_strength + 0.25 * military_strength);
    demand[trade_good_index(TradeGoodKind::ProcessedMinerals)] = m * (0.05 + 0.55 * mining_strength);
    demand[trade_good_index(TradeGoodKind::Fuel)] = m * (0.08 + 0.55 * mining_strength + 0.45 * military_strength + 0.10 * hub);
    demand[trade_good_index(TradeGoodKind::Munitions)] = m * (0.05 + 0.95 * military_strength);

    // Colony signals.
    if (opt.include_colony_contributions) {
      for (int k = 0; k < kTradeGoodKindCount; ++k) {
        supply[k] += colony_supply[i][k];
        demand[k] += colony_demand[i][k];
      }
    }

    TradeNode node;
    node.system_id = sid;
    node.market_size = m;
    node.hub_score = hub;
    node.supply = supply;
    node.demand = demand;

    // Balance + primary import/export.
    int best_exp = 0;
    int best_imp = 0;
    double best_exp_v = 0.0;
    double best_imp_v = 0.0;
    for (int k = 0; k < kTradeGoodKindCount; ++k) {
      node.balance[k] = supply[k] - demand[k];
      net_balance[i][k] = node.balance[k];
      if (node.balance[k] > best_exp_v + 1e-12) {
        best_exp_v = node.balance[k];
        best_exp = k;
      }
      if (-node.balance[k] > best_imp_v + 1e-12) {
        best_imp_v = -node.balance[k];
        best_imp = k;
      }
    }
    node.primary_export = static_cast<TradeGoodKind>(best_exp);
    node.primary_import = static_cast<TradeGoodKind>(best_imp);

    out.nodes.push_back(std::move(node));
  }

  if (opt.max_lanes <= 0) {
    // Node summaries are still useful for UI overlays and economy hooks, but
    // lane computation can be expensive (all-pairs shortest paths).
    return out;
  }

  // Jump-graph distances for travel-cost decay.
  const auto dist = jump_graph_distances(s, system_ids);

  // Lanes.
  struct LaneKey {
    int from{0};
    int to{0};
    bool operator==(const LaneKey& o) const noexcept { return from == o.from && to == o.to; }
  };
  struct LaneKeyHash {
    std::size_t operator()(const LaneKey& k) const noexcept {
      // pair hash
      const std::size_t h1 = std::hash<int>{}(k.from);
      const std::size_t h2 = std::hash<int>{}(k.to);
      return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
  };
  struct LaneAgg {
    double total{0.0};
    std::array<double, kTradeGoodKindCount> goods{};
  };

  std::unordered_map<LaneKey, LaneAgg, LaneKeyHash> lanes;
  lanes.reserve(static_cast<std::size_t>(n) * static_cast<std::size_t>(n / 2 + 1));

  const double expn = std::max(0.25, opt.distance_exponent);

  for (int i = 0; i < n; ++i) {
    for (int j = 0; j < n; ++j) {
      if (i == j) continue;
      const double d = dist[i][j];
      if (!std::isfinite(d)) continue;
      const double decay = 1.0 / std::pow(d + 1.0, expn);
      const double hub_boost = 1.0 + 0.25 * (hub_score[i] + hub_score[j]);

      bool any = false;
      LaneAgg agg;
      for (int k = 0; k < kTradeGoodKindCount; ++k) {
        const double exp = std::max(0.0, net_balance[i][k]);
        const double imp = std::max(0.0, -net_balance[j][k]);
        if (exp <= 1e-12 || imp <= 1e-12) continue;
        const double v = exp * imp * decay * hub_boost;
        if (v <= 1e-12) continue;
        agg.total += v;
        agg.goods[k] += v;
        any = true;
      }
      if (!any || agg.total <= 1e-12) continue;

      LaneKey key{i, j};
      LaneAgg& dst = lanes[key];
      dst.total += agg.total;
      for (int k = 0; k < kTradeGoodKindCount; ++k) dst.goods[k] += agg.goods[k];
    }
  }

  if (!lanes.empty()) {
    struct LaneItem {
      int from{0};
      int to{0};
      LaneAgg agg;
    };
    std::vector<LaneItem> items;
    items.reserve(lanes.size());
    for (const auto& kv : lanes) {
      items.push_back(LaneItem{kv.first.from, kv.first.to, kv.second});
    }

    std::sort(items.begin(), items.end(), [&](const LaneItem& a, const LaneItem& b) {
      if (a.agg.total > b.agg.total + 1e-12) return true;
      if (b.agg.total > a.agg.total + 1e-12) return false;
      if (system_ids[a.from] != system_ids[b.from]) return system_ids[a.from] < system_ids[b.from];
      return system_ids[a.to] < system_ids[b.to];
    });

    if (static_cast<int>(items.size()) > opt.max_lanes) items.resize(std::max(0, opt.max_lanes));

    out.lanes.reserve(items.size());
    for (const auto& it : items) {
      TradeLane lane;
      lane.from_system_id = system_ids[it.from];
      lane.to_system_id = system_ids[it.to];
      lane.total_volume = it.agg.total;

      // Top goods.
      std::vector<std::pair<int, double>> gv;
      gv.reserve(kTradeGoodKindCount);
      for (int k = 0; k < kTradeGoodKindCount; ++k) {
        if (it.agg.goods[k] <= 1e-12) continue;
        gv.push_back({k, it.agg.goods[k]});
      }
      std::sort(gv.begin(), gv.end(), [](const auto& a, const auto& b) {
        if (a.second > b.second + 1e-12) return true;
        if (b.second > a.second + 1e-12) return false;
        return a.first < b.first;
      });
      if (static_cast<int>(gv.size()) > opt.max_goods_per_lane) gv.resize(std::max(0, opt.max_goods_per_lane));

      lane.top_flows.reserve(gv.size());
      for (const auto& [k, v] : gv) {
        lane.top_flows.push_back(TradeGoodFlow{static_cast<TradeGoodKind>(k), v});
      }
      out.lanes.push_back(std::move(lane));
    }
  }

  return out;
}

}  // namespace nebula4x
