#include <algorithm>
#include <array>
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

double total_body_minerals(const nebula4x::GameState& s) {
  double total = 0.0;
  for (const auto& [bid, b] : s.bodies) {
    (void)bid;
    for (const auto& [k, v] : b.mineral_deposits) {
      (void)k;
      if (std::isfinite(v) && v > 0.0) total += v;
    }
  }
  return total;
}

struct AnomalyProfile {
  int count{0};
  double avg_dist_norm{0.0};
  double avg_hazard{0.0};
};

AnomalyProfile anomaly_profile(const nebula4x::GameState& s) {
  AnomalyProfile out;
  if (s.systems.empty()) return out;

  const nebula4x::Id home =
      (s.selected_system != nebula4x::kInvalidId && nebula4x::find_ptr(s.systems, s.selected_system) != nullptr)
          ? s.selected_system
          : s.systems.begin()->first;
  const auto* hs = nebula4x::find_ptr(s.systems, home);
  if (!hs) return out;

  const nebula4x::Vec2 home_pos = hs->galaxy_pos;
  double max_d = 1e-6;
  for (const auto& [sid, sys] : s.systems) {
    (void)sid;
    max_d = std::max(max_d, (sys.galaxy_pos - home_pos).length());
  }

  double dist_sum = 0.0;
  double hazard_sum = 0.0;
  for (const auto& [aid, a] : s.anomalies) {
    (void)aid;
    const auto* sys = nebula4x::find_ptr(s.systems, a.system_id);
    if (!sys) continue;
    const double dn = std::clamp((sys->galaxy_pos - home_pos).length() / max_d, 0.0, 1.0);
    dist_sum += dn;
    hazard_sum += std::max(0.0, a.hazard_chance) * std::max(0.0, a.hazard_damage);
    ++out.count;
  }

  if (out.count > 0) {
    out.avg_dist_norm = dist_sum / static_cast<double>(out.count);
    out.avg_hazard = hazard_sum / static_cast<double>(out.count);
  }
  return out;
}

bool run_matrix_case(int shape, int placement, int style, double dens, bool enable_regions,
                     std::uint32_t case_seed, int case_systems) {
  auto fail = [&](const char* why) {
    std::cerr << "matrix case failed: " << why
              << " (shape=" << shape
              << ", placement=" << placement
              << ", style=" << style
              << ", dens=" << dens
              << ", regions=" << (enable_regions ? "true" : "false")
              << ", seed=" << case_seed
              << ", systems=" << case_systems << ")\n";
    return false;
  };

  nebula4x::RandomScenarioConfig cfg;
  cfg.seed = case_seed;
  cfg.num_systems = case_systems;
  cfg.galaxy_shape = static_cast<nebula4x::RandomGalaxyShape>(shape);
  cfg.placement_style = static_cast<nebula4x::RandomPlacementStyle>(placement);
  cfg.placement_quality = 12;
  cfg.jump_network_style = static_cast<nebula4x::RandomJumpNetworkStyle>(style);
  cfg.jump_density = dens;
  cfg.enable_regions = enable_regions;
  cfg.num_regions = -1;

  const auto a = nebula4x::make_random_scenario(cfg);
  if (style == 0 && enable_regions) {
    // Determinism is already covered above; keep one representative matrix
    // case as an additional guard while avoiding repeated heavy serialization.
    const auto b = nebula4x::make_random_scenario(cfg);
    if (nebula4x::serialize_game_to_json(a) != nebula4x::serialize_game_to_json(b)) {
      return fail("determinism mismatch");
    }
  }

  if (!jump_network_connected(a)) {
    return fail("jump network disconnected");
  }

  // The PlanarProximity archetype should never introduce straight-edge crossings.
  if (style == static_cast<int>(nebula4x::RandomJumpNetworkStyle::PlanarProximity) &&
      count_jump_edge_crossings(a) != 0) {
    return fail("planar proximity crossing detected");
  }

  // Jump points should remain bi-directional.
  for (const auto& [id, jp] : a.jump_points) {
    const auto* other = nebula4x::find_ptr(a.jump_points, jp.linked_jump_id);
    if (!other) return fail("missing linked jump point");
    if (other->linked_jump_id != id) return fail("jump backlink mismatch");
  }

  // Region consistency.
  if (enable_regions) {
    if (a.regions.empty()) return fail("regions missing");
    for (const auto& [sid, sys] : a.systems) {
      (void)sid;
      if (sys.region_id == nebula4x::kInvalidId) return fail("system missing region id");
      if (!nebula4x::find_ptr(a.regions, sys.region_id)) return fail("system points to missing region");
    }
  } else {
    if (!a.regions.empty()) return fail("regions unexpectedly present");
    for (const auto& [sid, sys] : a.systems) {
      (void)sid;
      if (sys.region_id != nebula4x::kInvalidId) return fail("region id present when regions disabled");
    }
  }

  return true;
}

} // namespace

int test_random_scenario() {
  const std::uint32_t seed = 12345;
  const int n = 6;

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


  // Homeworld oxygen should be present and sane when generated.
  nebula4x::Id terran_id = nebula4x::kInvalidId;
  for (const auto& [fid, f] : s1.factions) {
    if (f.name.find("Terran") != std::string::npos) {
      terran_id = fid;
      break;
    }
  }
  N4X_ASSERT(terran_id != nebula4x::kInvalidId);

  const nebula4x::Colony* terran_colony = nullptr;
  for (const auto& [cid, c] : s1.colonies) {
    if (c.faction_id == terran_id) {
      terran_colony = &c;
      break;
    }
  }
  N4X_ASSERT(terran_colony != nullptr);

  const nebula4x::Body* home = nebula4x::find_ptr(s1.bodies, terran_colony->body_id);
  N4X_ASSERT(home != nullptr);
  if (home->type == nebula4x::BodyType::Planet || home->type == nebula4x::BodyType::Moon) {
    N4X_ASSERT(home->atmosphere_atm > 0.0);
    N4X_ASSERT(home->oxygen_atm > 0.10);
    N4X_ASSERT(home->oxygen_atm <= home->atmosphere_atm + 1e-9);
    N4X_ASSERT(home->terraforming_target_o2_atm > 0.0);
    N4X_ASSERT(std::fabs(home->terraforming_target_o2_atm - home->oxygen_atm) < 1e-6);
  }
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

  // Procedural alien species / empire profiles should be generated for AI empires.
  auto validate_ai_species = [](const nebula4x::GameState& st) {
    std::unordered_set<std::string> species_names;
    int ai_empires = 0;
    for (const auto& [fid, f] : st.factions) {
      (void)fid;
      if (f.control != nebula4x::FactionControl::AI_Explorer) continue;
      ++ai_empires;
      N4X_ASSERT(!f.species.name.empty());
      N4X_ASSERT(!f.species.adjective.empty());
      N4X_ASSERT(!f.species.archetype.empty());
      N4X_ASSERT(!f.species.ethos.empty());
      N4X_ASSERT(!f.species.government.empty());
      N4X_ASSERT(f.species.ideal_temp_k > 0.0);
      N4X_ASSERT(f.species.ideal_atm > 0.0);
      N4X_ASSERT(f.species.ideal_o2_atm > 0.0);
      N4X_ASSERT(species_names.insert(f.species.name).second);

      auto deviates = [](double v) { return std::abs(v - 1.0) > 1e-3; };
      const auto& tr = f.traits;
      N4X_ASSERT(tr.mining > 0.0 && tr.industry > 0.0 && tr.research > 0.0 && tr.construction > 0.0);
      N4X_ASSERT(tr.shipyard > 0.0 && tr.terraforming > 0.0 && tr.pop_growth > 0.0 && tr.troop_training > 0.0);
      const bool has_trait = deviates(tr.mining) || deviates(tr.industry) || deviates(tr.research) ||
                             deviates(tr.construction) || deviates(tr.shipyard) || deviates(tr.terraforming) ||
                             deviates(tr.pop_growth) || deviates(tr.troop_training);
      N4X_ASSERT(has_trait);
    }
    return ai_empires;
  };

  // Auto-scaled scenarios may legitimately choose zero AI empires for some sizes/seeds.
  (void)validate_ai_species(s1);

  // Explicitly forcing AI empires should produce at least one AI_Explorer with valid species/traits.
  {
    nebula4x::RandomScenarioConfig cfg;
    cfg.seed = seed;
    cfg.num_systems = n;
    cfg.num_ai_empires = 2;
    const auto s_ai = nebula4x::make_random_scenario(cfg);
    N4X_ASSERT(validate_ai_species(s_ai) >= 1);
  }

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

  // Resource abundance should scale procedural mineral totals meaningfully.
  {
    nebula4x::RandomScenarioConfig low_res;
    low_res.seed = seed + 42u;
    low_res.num_systems = 20;
    low_res.resource_abundance = 0.6;

    nebula4x::RandomScenarioConfig hi_res = low_res;
    hi_res.resource_abundance = 1.8;

    const auto s_low_res = nebula4x::make_random_scenario(low_res);
    const auto s_hi_res = nebula4x::make_random_scenario(hi_res);
    const double m_low = total_body_minerals(s_low_res);
    const double m_hi = total_body_minerals(s_hi_res);
    N4X_ASSERT(m_low > 0.0);
    N4X_ASSERT(m_hi > m_low * 2.5);
  }

  // Frontier intensity should push anomalies outward and increase hazard pressure.
  {
    nebula4x::RandomScenarioConfig low_frontier;
    low_frontier.seed = seed + 99u;
    low_frontier.num_systems = 28;
    low_frontier.frontier_intensity = 0.6;

    nebula4x::RandomScenarioConfig hi_frontier = low_frontier;
    hi_frontier.frontier_intensity = 1.8;

    const auto s_low_frontier = nebula4x::make_random_scenario(low_frontier);
    const auto s_hi_frontier = nebula4x::make_random_scenario(hi_frontier);
    const AnomalyProfile p_low = anomaly_profile(s_low_frontier);
    const AnomalyProfile p_hi = anomaly_profile(s_hi_frontier);

    N4X_ASSERT(p_low.count > 0);
    N4X_ASSERT(p_hi.count >= p_low.count);
    N4X_ASSERT(p_hi.avg_dist_norm >= p_low.avg_dist_norm);
    N4X_ASSERT(p_hi.avg_hazard >= p_low.avg_hazard * 0.90);
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

  // Bounded smoke-matrix:
  // 1) all jump-network styles x both region modes
  // 2) a shape sweep to cover all galaxy shapes
  // This keeps coverage broad while capping runtime for isolated test execution.

  const std::array<double, 3> densities = {0.0, 1.0, 2.0};
  for (int style = 0; style <= 6; ++style) {
    for (bool enable_regions : {false, true}) {
      const int shape = style % 6;       // covers all 6 galaxy shapes across styles
      const int placement = style % 2;   // exercises both placement modes
      const double dens = densities[static_cast<std::size_t>(style % static_cast<int>(densities.size()))];
      const std::uint32_t case_seed = seed + 1000u + static_cast<std::uint32_t>(style * 37 + (enable_regions ? 1 : 0));
      N4X_ASSERT(run_matrix_case(shape, placement, style, dens, enable_regions, case_seed, 7));
    }
  }

  // Explicit shape sweep (fixed jump style) to ensure all shapes are exercised directly.
  for (int shape = 0; shape <= 5; ++shape) {
    const int placement = shape % 2;
    const std::uint32_t case_seed = seed + 5000u + static_cast<std::uint32_t>(shape * 11);
    N4X_ASSERT(run_matrix_case(shape, placement, 0, 1.0, true, case_seed, 7));
  }

  // Stress regression: clustered jump style can create empty k-means buckets
  // under some seeds/sizes. Exercise a denser set of medium maps.
  for (int i = 0; i < 8; ++i) {
    const std::uint32_t case_seed = seed + 9000u + static_cast<std::uint32_t>(i * 17);
    N4X_ASSERT(run_matrix_case(
        static_cast<int>(nebula4x::RandomGalaxyShape::Clustered),
        static_cast<int>(nebula4x::RandomPlacementStyle::BlueNoise),
        static_cast<int>(nebula4x::RandomJumpNetworkStyle::ClusterBridges),
        1.4, true, case_seed, 24));
  }

  return 0;
}
