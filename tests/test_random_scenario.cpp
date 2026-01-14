#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "nebula4x/core/serialization.h"
#include "nebula4x/core/scenario.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n";           \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

namespace {

// Basic graph connectivity check over the jump network.
bool jump_network_connected(const nebula4x::GameState& s) {
  if (s.systems.empty()) return true;
  const nebula4x::Id start = (s.selected_system != nebula4x::kInvalidId) ? s.selected_system : s.systems.begin()->first;

  std::unordered_set<nebula4x::Id> visited;
  std::queue<nebula4x::Id> q;
  visited.insert(start);
  q.push(start);

  while (!q.empty()) {
    const auto cur = q.front();
    q.pop();
    const auto* sys = nebula4x::find_ptr(s.systems, cur);
    if (!sys) continue;
    for (const auto jp_id : sys->jump_points) {
      const auto* jp = nebula4x::find_ptr(s.jump_points, jp_id);
      if (!jp) continue;
      const auto* other = nebula4x::find_ptr(s.jump_points, jp->linked_jump_id);
      if (!other) continue;
      const auto next_sys = other->system_id;
      if (next_sys == nebula4x::kInvalidId) continue;
      if (visited.insert(next_sys).second) q.push(next_sys);
    }
  }

  return visited.size() == s.systems.size();
}

// Count strict geometric crossings between undirected jump edges (galaxy-space
// straight segments). Shared endpoints are ignored.
int count_jump_edge_crossings(const nebula4x::GameState& s) {
  struct PairHash {
    std::size_t operator()(const std::pair<nebula4x::Id, nebula4x::Id>& p) const noexcept {
      return std::hash<nebula4x::Id>{}(p.first) ^ (std::hash<nebula4x::Id>{}(p.second) << 1);
    }
  };

  std::unordered_map<nebula4x::Id, nebula4x::Vec2> pos;
  pos.reserve(s.systems.size() * 2);
  for (const auto& [sid, sys] : s.systems) pos[sid] = sys.galaxy_pos;

  std::unordered_set<std::pair<nebula4x::Id, nebula4x::Id>, PairHash> edges;
  edges.reserve(s.jump_points.size());
  for (const auto& [_, jp] : s.jump_points) {
    const auto* other = nebula4x::find_ptr(s.jump_points, jp.linked_jump_id);
    if (!other) continue;
    nebula4x::Id a = jp.system_id;
    nebula4x::Id b = other->system_id;
    if (a == nebula4x::kInvalidId || b == nebula4x::kInvalidId || a == b) continue;
    if (a > b) std::swap(a, b);
    edges.insert({a, b});
  }

  std::vector<std::pair<nebula4x::Id, nebula4x::Id>> el;
  el.reserve(edges.size());
  for (const auto& e : edges) el.push_back(e);

  auto orient = [](const nebula4x::Vec2& a, const nebula4x::Vec2& b, const nebula4x::Vec2& c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
  };
  auto proper_intersect = [&](const nebula4x::Vec2& a, const nebula4x::Vec2& b,
                              const nebula4x::Vec2& c, const nebula4x::Vec2& d) {
    const double o1 = orient(a, b, c);
    const double o2 = orient(a, b, d);
    const double o3 = orient(c, d, a);
    const double o4 = orient(c, d, b);
    const double eps = 1e-12;
    if (std::fabs(o1) < eps || std::fabs(o2) < eps || std::fabs(o3) < eps || std::fabs(o4) < eps) return false;
    return (o1 * o2 < 0.0) && (o3 * o4 < 0.0);
  };

  int crossings = 0;
  for (std::size_t i = 0; i < el.size(); ++i) {
    const auto [a_id, b_id] = el[i];
    const auto ita = pos.find(a_id);
    const auto itb = pos.find(b_id);
    if (ita == pos.end() || itb == pos.end()) continue;
    const nebula4x::Vec2 a = ita->second;
    const nebula4x::Vec2 b = itb->second;
    for (std::size_t j = i + 1; j < el.size(); ++j) {
      const auto [c_id, d_id] = el[j];
      if (a_id == c_id || a_id == d_id || b_id == c_id || b_id == d_id) continue;
      const auto itc = pos.find(c_id);
      const auto itd = pos.find(d_id);
      if (itc == pos.end() || itd == pos.end()) continue;
      if (proper_intersect(a, b, itc->second, itd->second)) ++crossings;
    }
  }

  return crossings;
}

} // namespace

int test_random_scenario() {
  const std::uint32_t seed = 12345;
  const int n = 10;

  const auto s1 = nebula4x::make_random_scenario(seed, n);
  const auto s2 = nebula4x::make_random_scenario(seed, n);
  const auto s3 = nebula4x::make_random_scenario(seed + 1, n);

  // Deterministic generation for the same (seed,n).
  const std::string j1 = nebula4x::serialize_game_to_json(s1);
  const std::string j2 = nebula4x::serialize_game_to_json(s2);
  N4X_ASSERT(j1 == j2);

  // A different seed should (very likely) differ.
  const std::string j3 = nebula4x::serialize_game_to_json(s3);
  N4X_ASSERT(j1 != j3);

  // Basic invariants.
  N4X_ASSERT(static_cast<int>(s1.systems.size()) == n);
  N4X_ASSERT(!s1.bodies.empty());
  N4X_ASSERT(!s1.colonies.empty());
  N4X_ASSERT(!s1.ships.empty());
  N4X_ASSERT(!s1.jump_points.empty());

  // Independent outposts are enabled by default and should create at least one
  // AI_Passive minor faction with seeded colonies.
  nebula4x::Id indep_id = nebula4x::kInvalidId;
  for (const auto& [fid, f] : s1.factions) {
    (void)fid;
    if (f.control == nebula4x::FactionControl::AI_Passive && f.name == "Independent Worlds") {
      indep_id = fid;
      break;
    }
  }
  N4X_ASSERT(indep_id != nebula4x::kInvalidId);
  bool has_indep_colony = false;
  for (const auto& [cid, c] : s1.colonies) {
    (void)cid;
    if (c.faction_id == indep_id) {
      has_indep_colony = true;
      break;
    }
  }
  N4X_ASSERT(has_indep_colony);

  // Config toggle: disable independents.
  nebula4x::RandomScenarioConfig noind;
  noind.seed = seed;
  noind.num_systems = n;
  noind.enable_independents = false;
  const auto s_noind = nebula4x::make_random_scenario(noind);
  for (const auto& [fid, f] : s_noind.factions) {
    (void)fid;
    N4X_ASSERT(!(f.control == nebula4x::FactionControl::AI_Passive && f.name == "Independent Worlds"));
  }

  // Regions invariants (enabled by default in RandomScenarioConfig).
  N4X_ASSERT(!s1.regions.empty());
  {
    std::unordered_set<std::string> names;
    for (const auto& [rid, reg] : s1.regions) {
      (void)rid;
      N4X_ASSERT(!reg.name.empty());
      names.insert(reg.name);
    }
    N4X_ASSERT(names.size() == s1.regions.size());
  }
  for (const auto& [sid, sys] : s1.systems) {
    (void)sid;
    N4X_ASSERT(sys.region_id != nebula4x::kInvalidId);
    N4X_ASSERT(nebula4x::find_ptr(s1.regions, sys.region_id) != nullptr);
  }

  // Jump points should be bi-directionally linked.
  for (const auto& [id, jp] : s1.jump_points) {
    const auto* other = nebula4x::find_ptr(s1.jump_points, jp.linked_jump_id);
    N4X_ASSERT(other != nullptr);
    N4X_ASSERT(other->linked_jump_id == id);
    N4X_ASSERT(nebula4x::find_ptr(s1.systems, jp.system_id) != nullptr);
  }

  // ...and the graph should be connected.
  N4X_ASSERT(jump_network_connected(s1));

  // Smoke-test all jump network styles/densities, galaxy shapes, and placement
  // modes for determinism + connectivity.
  for (int shape = 0; shape <= 4; ++shape) {
    for (int placement = 0; placement <= 1; ++placement) {
      for (int style = 0; style <= 5; ++style) {
        for (double dens : {0.0, 1.0, 2.0}) {
          for (bool enable_regions : {false, true}) {
            nebula4x::RandomScenarioConfig cfg;
            cfg.seed = seed;
            cfg.num_systems = n;
            cfg.galaxy_shape = static_cast<nebula4x::RandomGalaxyShape>(shape);
            cfg.placement_style = static_cast<nebula4x::RandomPlacementStyle>(placement);
            cfg.placement_quality = 24;
            cfg.jump_network_style = static_cast<nebula4x::RandomJumpNetworkStyle>(style);
            cfg.jump_density = dens;
            cfg.enable_regions = enable_regions;
            cfg.num_regions = -1;

            const auto a = nebula4x::make_random_scenario(cfg);
            const auto b = nebula4x::make_random_scenario(cfg);

            // Deterministic for identical config.
            N4X_ASSERT(nebula4x::serialize_game_to_json(a) == nebula4x::serialize_game_to_json(b));

            // Connected jump graph for all archetypes.
            N4X_ASSERT(jump_network_connected(a));

            // The PlanarProximity archetype should never introduce straight-edge crossings.
            if (style == static_cast<int>(nebula4x::RandomJumpNetworkStyle::PlanarProximity)) {
              N4X_ASSERT(count_jump_edge_crossings(a) == 0);
            }

            // Jump points should remain bi-directional.
            for (const auto& [id, jp] : a.jump_points) {
              const auto* other = nebula4x::find_ptr(a.jump_points, jp.linked_jump_id);
              N4X_ASSERT(other != nullptr);
              N4X_ASSERT(other->linked_jump_id == id);
            }

            // Region consistency.
            if (enable_regions) {
              N4X_ASSERT(!a.regions.empty());
              for (const auto& [sid, sys] : a.systems) {
                (void)sid;
                N4X_ASSERT(sys.region_id != nebula4x::kInvalidId);
                N4X_ASSERT(nebula4x::find_ptr(a.regions, sys.region_id) != nullptr);
              }
            } else {
              N4X_ASSERT(a.regions.empty());
              for (const auto& [sid, sys] : a.systems) {
                (void)sid;
                N4X_ASSERT(sys.region_id == nebula4x::kInvalidId);
              }
            }
          }
        }
      }
    }
  }

  return 0;
}
