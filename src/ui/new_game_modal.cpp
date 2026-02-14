#include "ui/new_game_modal.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <future>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <queue>
#include <vector>
#include <limits>

#include "nebula4x/core/scenario.h"
#include "nebula4x/util/log.h"
#include "ui/procgen_graphics.h"

namespace nebula4x::ui {
namespace {

constexpr int kScenarioSol = 0;
constexpr int kScenarioRandom = 1;
constexpr int kMaxRandomSystems = 300;

// Responsiveness guardrails for large procedural previews/searches.
constexpr int kAutoPreviewProxySystemCap = 180;
constexpr int kMetricParallelMinSystems = 140;
constexpr double kSeedSearchFrameBudgetMs = 14.0;
constexpr double kSeedSearchSessionBudgetBaseMs = 5000.0;
constexpr double kSeedSearchSessionBudgetPerTryMs = 18.0;
constexpr double kSeedSearchSessionBudgetPerSystemMs = 4.0;
constexpr double kSeedSearchPerCandidateHardMs = 1800.0;
constexpr int kSeedSearchProxySystems = 160;
constexpr int kSeedSearchProxyActivateAtSystems = 220;
constexpr int kStarPlacementExactNodeCap = 180;
constexpr int kJumpStatsExactNodeCap = 170;
constexpr int kJumpStatsExactEdgeCap = 420;
constexpr std::size_t kJumpCrossingSamplePairs = 60000;
constexpr int kJumpDiameterApproxSeedCount = 6;

double elapsed_ms(const std::chrono::steady_clock::time_point start,
                  const std::chrono::steady_clock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

std::uint32_t time_seed_u32() {
  using namespace std::chrono;
  const std::uint64_t t = static_cast<std::uint64_t>(
      duration_cast<nanoseconds>(high_resolution_clock::now().time_since_epoch()).count());
  // Mix bits a bit to avoid obvious patterns when only low bits change.
  std::uint64_t x = t;
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return static_cast<std::uint32_t>(x ^ (x >> 32));
}

// --- Seed mixing helpers (UI-side) ---
//
// The procedural generator is deterministic given a seed. The UI offers a
// "seed explorer" that tries a bunch of seeds and keeps the best one.
// We want candidate seeds to be well distributed even if the base seed only
// increments by 1.
std::uint64_t splitmix64(std::uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

std::uint32_t mix_seed(std::uint32_t base, std::uint64_t i) {
  const std::uint64_t x = (static_cast<std::uint64_t>(base) << 32) ^ (i * 0x9e3779b97f4a7c15ULL);
  return static_cast<std::uint32_t>(splitmix64(x) & 0xffffffffu);
}

std::uint64_t config_signature_no_seed(const nebula4x::RandomScenarioConfig& cfg) {
  std::uint64_t h = 0x6a09e667f3bcc909ULL;
  auto add = [&](std::uint64_t v) {
    h ^= splitmix64(v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
  };

  add(static_cast<std::uint64_t>(cfg.num_systems));
  add(static_cast<std::uint64_t>(static_cast<int>(cfg.galaxy_shape)));
  add(static_cast<std::uint64_t>(static_cast<int>(cfg.placement_style)));
  add(static_cast<std::uint64_t>(cfg.placement_quality));
  add(static_cast<std::uint64_t>(static_cast<int>(cfg.jump_network_style)));
  add(static_cast<std::uint64_t>(std::llround(cfg.jump_density * 1000.0)));
  add(static_cast<std::uint64_t>(std::llround(cfg.resource_abundance * 1000.0)));
  add(static_cast<std::uint64_t>(std::llround(cfg.frontier_intensity * 1000.0)));
  add(static_cast<std::uint64_t>(std::llround(cfg.xenoarchaeology_spawn_pressure_early * 1000.0)));
  add(static_cast<std::uint64_t>(std::llround(cfg.xenoarchaeology_spawn_pressure_late * 1000.0)));

  add(static_cast<std::uint64_t>(cfg.enable_regions ? 1 : 0));
  add(static_cast<std::uint64_t>(cfg.num_regions + 100));

  add(static_cast<std::uint64_t>(cfg.num_ai_empires + 100));
  add(static_cast<std::uint64_t>(cfg.enable_pirates ? 1 : 0));
  add(static_cast<std::uint64_t>(std::llround(cfg.pirate_strength * 1000.0)));

  add(static_cast<std::uint64_t>(cfg.enable_independents ? 1 : 0));
  add(static_cast<std::uint64_t>(cfg.num_independent_outposts + 100));
  add(static_cast<std::uint64_t>(cfg.ensure_clear_home ? 1 : 0));
  return h;
}

nebula4x::RandomScenarioConfig sanitize_random_config(nebula4x::RandomScenarioConfig cfg) {
  cfg.num_systems = std::clamp(cfg.num_systems, 1, kMaxRandomSystems);
  cfg.galaxy_shape =
      static_cast<nebula4x::RandomGalaxyShape>(std::clamp(static_cast<int>(cfg.galaxy_shape), 0, 5));
  cfg.placement_style =
      static_cast<nebula4x::RandomPlacementStyle>(std::clamp(static_cast<int>(cfg.placement_style), 0, 1));
  cfg.placement_quality = std::clamp(cfg.placement_quality, 4, 96);

  cfg.jump_network_style = static_cast<nebula4x::RandomJumpNetworkStyle>(
      std::clamp(static_cast<int>(cfg.jump_network_style), 0, 6));
  if (!std::isfinite(cfg.jump_density)) cfg.jump_density = 1.0;
  cfg.jump_density = std::clamp(cfg.jump_density, 0.0, 2.0);
  if (!std::isfinite(cfg.resource_abundance)) cfg.resource_abundance = 1.0;
  cfg.resource_abundance = std::clamp(cfg.resource_abundance, 0.5, 2.0);
  if (!std::isfinite(cfg.frontier_intensity)) cfg.frontier_intensity = 1.0;
  cfg.frontier_intensity = std::clamp(cfg.frontier_intensity, 0.5, 2.0);
  if (!std::isfinite(cfg.xenoarchaeology_spawn_pressure_early)) cfg.xenoarchaeology_spawn_pressure_early = 1.0;
  if (!std::isfinite(cfg.xenoarchaeology_spawn_pressure_late)) cfg.xenoarchaeology_spawn_pressure_late = 1.0;
  cfg.xenoarchaeology_spawn_pressure_early = std::clamp(cfg.xenoarchaeology_spawn_pressure_early, 0.25, 3.0);
  cfg.xenoarchaeology_spawn_pressure_late = std::clamp(cfg.xenoarchaeology_spawn_pressure_late, 0.25, 3.0);

  cfg.num_regions = std::clamp(cfg.num_regions, -1, 12);
  cfg.num_ai_empires = std::clamp(cfg.num_ai_empires, -1, 12);

  if (!std::isfinite(cfg.pirate_strength)) cfg.pirate_strength = 1.0;
  cfg.pirate_strength = std::clamp(cfg.pirate_strength, 0.0, 5.0);

  cfg.num_independent_outposts = std::clamp(cfg.num_independent_outposts, -1, 64);
  return cfg;
}

nebula4x::RandomScenarioConfig random_config_from_ui(const UIState& ui) {
  nebula4x::RandomScenarioConfig cfg;
  cfg.seed = ui.new_game_random_seed;
  cfg.num_systems = ui.new_game_random_num_systems;
  cfg.galaxy_shape =
      static_cast<nebula4x::RandomGalaxyShape>(std::clamp(ui.new_game_random_galaxy_shape, 0, 5));
  cfg.placement_style =
      static_cast<nebula4x::RandomPlacementStyle>(std::clamp(ui.new_game_random_placement_style, 0, 1));
  cfg.placement_quality = std::clamp(ui.new_game_random_placement_quality, 4, 96);
  cfg.jump_network_style =
      static_cast<nebula4x::RandomJumpNetworkStyle>(std::clamp(ui.new_game_random_jump_network_style, 0, 6));
  cfg.jump_density = static_cast<double>(ui.new_game_random_jump_density);
  cfg.resource_abundance = static_cast<double>(ui.new_game_random_resource_abundance);
  cfg.frontier_intensity = static_cast<double>(ui.new_game_random_frontier_intensity);
  cfg.xenoarchaeology_spawn_pressure_early = static_cast<double>(ui.new_game_random_xeno_spawn_pressure_early);
  cfg.xenoarchaeology_spawn_pressure_late = static_cast<double>(ui.new_game_random_xeno_spawn_pressure_late);

  cfg.enable_regions = ui.new_game_random_enable_regions;
  cfg.num_regions = ui.new_game_random_num_regions;

  cfg.num_ai_empires = ui.new_game_random_ai_empires;
  cfg.enable_pirates = ui.new_game_random_enable_pirates;
  cfg.pirate_strength = static_cast<double>(ui.new_game_random_pirate_strength);

  cfg.enable_independents = ui.new_game_random_enable_independents;
  cfg.num_independent_outposts = ui.new_game_random_num_independent_outposts;
  cfg.ensure_clear_home = ui.new_game_random_ensure_clear_home;
  return sanitize_random_config(cfg);
}

struct RandomPreviewCache {
  bool valid{false};
  std::uint32_t seed{0};
  int num_systems{0};
  int generated_systems{0};
  bool used_fast_proxy{false};
  double generation_ms{0.0};
  int galaxy_shape{0};
  int placement_style{0};
  int placement_quality{24};
  int jump_style{0};
  int jump_density_pct{100};
  int resource_abundance_pct{100};
  int frontier_intensity_pct{100};
  int xeno_spawn_pressure_early_pct{100};
  int xeno_spawn_pressure_late_pct{100};
  int ai_empires{0};
  bool enable_pirates{true};
  int pirate_strength_pct{100};
  bool enable_regions{true};
  int num_regions{-1};
  bool enable_independents{true};
  int num_independent_outposts{-1};
  bool ensure_clear_home{true};
  GameState state;
  std::string error;
};

void ensure_preview(RandomPreviewCache& cache, const nebula4x::RandomScenarioConfig& cfg_in, bool force_full_preview) {
  nebula4x::RandomScenarioConfig cfg = sanitize_random_config(cfg_in);
  nebula4x::RandomScenarioConfig gen_cfg = cfg;
  if (!force_full_preview && gen_cfg.num_systems > kAutoPreviewProxySystemCap) {
    gen_cfg.num_systems = kAutoPreviewProxySystemCap;
  }

  const int shape_i = std::clamp(static_cast<int>(cfg.galaxy_shape), 0, 5);
  const int placement_style_i = std::clamp(static_cast<int>(cfg.placement_style), 0, 1);
  const int placement_quality_i = std::clamp(cfg.placement_quality, 4, 96);
  const int jump_style_i = std::clamp(static_cast<int>(cfg.jump_network_style), 0, 6);
  const int strength_pct = static_cast<int>(std::llround(cfg.pirate_strength * 100.0));
  const int jump_density_pct = static_cast<int>(std::llround(cfg.jump_density * 100.0));
  const int resource_abundance_pct = static_cast<int>(std::llround(cfg.resource_abundance * 100.0));
  const int frontier_intensity_pct = static_cast<int>(std::llround(cfg.frontier_intensity * 100.0));
  const int xeno_spawn_pressure_early_pct = static_cast<int>(std::llround(cfg.xenoarchaeology_spawn_pressure_early * 100.0));
  const int xeno_spawn_pressure_late_pct = static_cast<int>(std::llround(cfg.xenoarchaeology_spawn_pressure_late * 100.0));

  if (cache.valid && cache.seed == cfg.seed && cache.num_systems == cfg.num_systems && cache.galaxy_shape == shape_i &&
      cache.placement_style == placement_style_i && cache.placement_quality == placement_quality_i &&
      cache.jump_style == jump_style_i && cache.jump_density_pct == jump_density_pct &&
      cache.resource_abundance_pct == resource_abundance_pct &&
      cache.frontier_intensity_pct == frontier_intensity_pct &&
      cache.xeno_spawn_pressure_early_pct == xeno_spawn_pressure_early_pct &&
      cache.xeno_spawn_pressure_late_pct == xeno_spawn_pressure_late_pct &&
      cache.ai_empires == cfg.num_ai_empires && cache.enable_pirates == cfg.enable_pirates &&
      cache.enable_regions == cfg.enable_regions && cache.num_regions == cfg.num_regions &&
      cache.pirate_strength_pct == strength_pct && cache.enable_independents == cfg.enable_independents &&
      cache.num_independent_outposts == cfg.num_independent_outposts && cache.ensure_clear_home == cfg.ensure_clear_home &&
      !(force_full_preview && cache.used_fast_proxy)) {
    return;
  }

  cache.valid = false;
  cache.seed = cfg.seed;
  cache.num_systems = cfg.num_systems;
  cache.generated_systems = 0;
  cache.used_fast_proxy = (gen_cfg.num_systems != cfg.num_systems);
  cache.generation_ms = 0.0;
  cache.galaxy_shape = shape_i;
  cache.placement_style = placement_style_i;
  cache.placement_quality = placement_quality_i;
  cache.jump_style = jump_style_i;
  cache.jump_density_pct = jump_density_pct;
  cache.resource_abundance_pct = resource_abundance_pct;
  cache.frontier_intensity_pct = frontier_intensity_pct;
  cache.xeno_spawn_pressure_early_pct = xeno_spawn_pressure_early_pct;
  cache.xeno_spawn_pressure_late_pct = xeno_spawn_pressure_late_pct;
  cache.ai_empires = cfg.num_ai_empires;
  cache.enable_pirates = cfg.enable_pirates;
  cache.pirate_strength_pct = strength_pct;
  cache.enable_regions = cfg.enable_regions;
  cache.num_regions = cfg.num_regions;
  cache.enable_independents = cfg.enable_independents;
  cache.num_independent_outposts = cfg.num_independent_outposts;
  cache.ensure_clear_home = cfg.ensure_clear_home;
  cache.error.clear();

  const auto t0 = std::chrono::steady_clock::now();
  try {
    cache.state = nebula4x::make_random_scenario(gen_cfg);
    const auto t1 = std::chrono::steady_clock::now();
    cache.generation_ms = elapsed_ms(t0, t1);
    cache.generated_systems = static_cast<int>(cache.state.systems.size());
    cache.valid = true;
  } catch (const std::exception& e) {
    cache.error = e.what();
    const auto t1 = std::chrono::steady_clock::now();
    cache.generation_ms = elapsed_ms(t0, t1);
  }
}


struct JumpGraphStats {
  int nodes{0};
  int undirected_edges{0};
  double avg_degree{0.0};
  double avg_edge_length{0.0};
  double edge_length_std{0.0};
  int edge_crossings{0};
  int diameter_hops{0};
  int articulation_points{0};
  int components{1};
  bool connected{true};

  // Stable index order for nodes used by the preview graph metrics.
  std::vector<Id> node_ids;

  // Connected component id for each node index (same length as node_ids).
  std::vector<int> component_of_node;

  // Articulation points reported as system ids.
  std::vector<Id> articulation_systems;
};

struct StarPlacementStats {
  int nodes{0};
  double min_nearest_neighbor{0.0};
  double avg_nearest_neighbor{0.0};
  double nearest_neighbor_std{0.0};
};

StarPlacementStats compute_star_placement_stats(const GameState& s) {
  StarPlacementStats st;
  st.nodes = static_cast<int>(s.systems.size());
  if (st.nodes <= 1) return st;

  std::vector<Vec2> pos;
  pos.reserve(s.systems.size());
  for (const auto& [_, sys] : s.systems) pos.push_back(sys.galaxy_pos);

  double min_nn = std::numeric_limits<double>::infinity();
  double sum = 0.0;
  double sum2 = 0.0;
  std::vector<std::size_t> sample_i;
  sample_i.reserve(pos.size());

  if (st.nodes > kStarPlacementExactNodeCap) {
    const std::size_t step =
        std::max<std::size_t>(1, (pos.size() + static_cast<std::size_t>(kStarPlacementExactNodeCap) - 1) /
                                     static_cast<std::size_t>(kStarPlacementExactNodeCap));
    for (std::size_t i = 0; i < pos.size(); i += step) sample_i.push_back(i);
    if (!sample_i.empty() && sample_i.back() != (pos.size() - 1)) sample_i.push_back(pos.size() - 1);
  } else {
    sample_i.resize(pos.size());
    for (std::size_t i = 0; i < pos.size(); ++i) sample_i[i] = i;
  }

  for (std::size_t i : sample_i) {
    double best = std::numeric_limits<double>::infinity();
    for (std::size_t j = 0; j < pos.size(); ++j) {
      if (i == j) continue;
      best = std::min(best, (pos[i] - pos[j]).length());
    }

    if (!std::isfinite(best)) best = 0.0;
    min_nn = std::min(min_nn, best);
    sum += best;
    sum2 += best * best;
  }

  const double m = static_cast<double>(sample_i.size());
  st.min_nearest_neighbor = std::isfinite(min_nn) ? min_nn : 0.0;
  st.avg_nearest_neighbor = sum / std::max(1e-9, m);
  const double var = std::max(0.0, (sum2 / std::max(1e-9, m)) - st.avg_nearest_neighbor * st.avg_nearest_neighbor);
  st.nearest_neighbor_std = std::sqrt(var);
  return st;
}

JumpGraphStats compute_jump_graph_stats(const GameState& s) {
  JumpGraphStats st;
  st.nodes = static_cast<int>(s.systems.size());
  if (st.nodes <= 1) {
    st.connected = true;
    return st;
  }

  // Build a stable indexing for the unordered system map.
  std::vector<Id> ids;
  ids.reserve(s.systems.size());
  for (const auto& [id, _] : s.systems) ids.push_back(id);

  // Ensure deterministic ordering across runs.
  std::sort(ids.begin(), ids.end());
  st.node_ids = ids;

  std::unordered_map<Id, int> idx;
  idx.reserve(ids.size() * 2);
  for (int i = 0; i < static_cast<int>(ids.size()); ++i) idx[ids[static_cast<std::size_t>(i)]] = i;

  std::vector<std::vector<int>> adj(ids.size());

  // Positions in a stable index order.
  std::vector<Vec2> pos;
  pos.reserve(ids.size());
  for (Id id : ids) {
    const auto* sys = nebula4x::find_ptr(s.systems, id);
    pos.push_back(sys ? sys->galaxy_pos : Vec2{0.0, 0.0});
  }

  // Deduplicate bi-directional jump points into undirected graph edges.
  std::unordered_set<std::uint64_t> edges;
  edges.reserve(s.jump_points.size() * 2);

  for (const auto& [_, jp] : s.jump_points) {
    const auto* other = nebula4x::find_ptr(s.jump_points, jp.linked_jump_id);
    if (!other) continue;
    const Id a = jp.system_id;
    const Id b = other->system_id;
    if (a == kInvalidId || b == kInvalidId) continue;

    const auto ita = idx.find(a);
    const auto itb = idx.find(b);
    if (ita == idx.end() || itb == idx.end()) continue;
    const int ia = ita->second;
    const int ib = itb->second;
    if (ia == ib) continue;

    const auto lo = static_cast<std::uint32_t>(std::min(ia, ib));
    const auto hi = static_cast<std::uint32_t>(std::max(ia, ib));
    const std::uint64_t key = (static_cast<std::uint64_t>(lo) << 32) | static_cast<std::uint64_t>(hi);
    if (!edges.insert(key).second) continue;

    adj[static_cast<std::size_t>(ia)].push_back(ib);
    adj[static_cast<std::size_t>(ib)].push_back(ia);
  }

  st.undirected_edges = static_cast<int>(edges.size());

  // Collect edge list for geometry metrics.
  std::vector<std::pair<int, int>> edge_list;
  edge_list.reserve(edges.size());
  for (const std::uint64_t k : edges) {
    const int a = static_cast<int>(k >> 32);
    const int b = static_cast<int>(k & 0xFFFFFFFFu);
    if (a == b) continue;
    if (a < 0 || b < 0 || a >= st.nodes || b >= st.nodes) continue;
    edge_list.emplace_back(a, b);
  }

  // Average edge length (galaxy units).
  if (!edge_list.empty()) {
    double sum = 0.0;
    double sum2 = 0.0;
    for (const auto& e : edge_list) {
      const Vec2 d = pos[static_cast<std::size_t>(e.first)] - pos[static_cast<std::size_t>(e.second)];
      const double len = d.length();
      sum += len;
      sum2 += len * len;
    }
    const double m = static_cast<double>(edge_list.size());
    st.avg_edge_length = sum / m;
    const double var = std::max(0.0, (sum2 / m) - st.avg_edge_length * st.avg_edge_length);
    st.edge_length_std = std::sqrt(var);
  }

  // Edge crossing count (strict segment intersection, ignoring shared endpoints).
  {
    auto orient = [](const Vec2& a, const Vec2& b, const Vec2& c) {
      return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    };
    auto proper_intersect = [&](const Vec2& a, const Vec2& b, const Vec2& c, const Vec2& d) {
      const double o1 = orient(a, b, c);
      const double o2 = orient(a, b, d);
      const double o3 = orient(c, d, a);
      const double o4 = orient(c, d, b);
      const double eps = 1e-12;
      // Ignore degenerate / collinear / touching cases; we only care about
      // true crossings that add visual clutter.
      if (std::fabs(o1) < eps || std::fabs(o2) < eps || std::fabs(o3) < eps || std::fabs(o4) < eps) return false;
      return (o1 * o2 < 0.0) && (o3 * o4 < 0.0);
    };

    auto edges_share_endpoint = [&](std::size_t i, std::size_t j) {
      const auto [a, b] = edge_list[i];
      const auto [c, d] = edge_list[j];
      return (a == c || a == d || b == c || b == d);
    };

    auto pair_crosses = [&](std::size_t i, std::size_t j) {
      const auto [a, b] = edge_list[i];
      const auto [c, d] = edge_list[j];
      const Vec2 pa = pos[static_cast<std::size_t>(a)];
      const Vec2 pb = pos[static_cast<std::size_t>(b)];
      const Vec2 pc = pos[static_cast<std::size_t>(c)];
      const Vec2 pd = pos[static_cast<std::size_t>(d)];
      return proper_intersect(pa, pb, pc, pd);
    };

    const std::size_t e = edge_list.size();
    const std::size_t total_pairs = (e >= 2) ? (e * (e - 1) / 2) : 0;
    std::size_t shared_pairs = 0;
    for (const auto& nbrs : adj) {
      const std::size_t d = nbrs.size();
      if (d >= 2) shared_pairs += d * (d - 1) / 2;
    }
    const std::size_t candidate_pairs = (total_pairs > shared_pairs) ? (total_pairs - shared_pairs) : 0;
    const bool exact_crossings = (candidate_pairs <= kJumpCrossingSamplePairs);

    if (exact_crossings) {
      int crossings = 0;
      for (std::size_t i = 0; i < e; ++i) {
        for (std::size_t j = i + 1; j < e; ++j) {
          if (edges_share_endpoint(i, j)) continue;
          if (pair_crosses(i, j)) ++crossings;
        }
      }
      st.edge_crossings = crossings;
    } else {
      // Deterministic pair sampling for large dense networks.
      std::uint64_t x = static_cast<std::uint64_t>(e) * 0x9E3779B97F4A7C15ULL + 0xD1B54A32D192ED03ULL;
      auto next_u64 = [&]() {
        x ^= (x >> 12);
        x ^= (x << 25);
        x ^= (x >> 27);
        return x * 0x2545F4914F6CDD1DULL;
      };

      const std::size_t sample_cap = kJumpCrossingSamplePairs;
      std::size_t sampled = 0;
      std::size_t valid = 0;
      std::size_t crossings = 0;
      std::size_t attempts = 0;

      std::unordered_set<std::uint64_t> used_pairs;
      used_pairs.reserve(sample_cap * 2 + 16);

      while (sampled < sample_cap && attempts < sample_cap * 8) {
        ++attempts;
        const std::size_t i = static_cast<std::size_t>(next_u64() % std::max<std::size_t>(1, e));
        const std::size_t j = static_cast<std::size_t>(next_u64() % std::max<std::size_t>(1, e));
        if (i == j) continue;

        const std::size_t lo = std::min(i, j);
        const std::size_t hi = std::max(i, j);
        const std::uint64_t key = (static_cast<std::uint64_t>(lo) << 32) | static_cast<std::uint64_t>(hi);
        if (!used_pairs.insert(key).second) continue;

        ++sampled;
        if (edges_share_endpoint(lo, hi)) continue;
        ++valid;
        if (pair_crosses(lo, hi)) ++crossings;
      }

      if (valid == 0 || candidate_pairs == 0) {
        st.edge_crossings = 0;
      } else {
        const double ratio = static_cast<double>(crossings) / static_cast<double>(valid);
        st.edge_crossings = static_cast<int>(std::llround(ratio * static_cast<double>(candidate_pairs)));
      }
    }
  }

  int sum_deg = 0;
  for (const auto& v : adj) sum_deg += static_cast<int>(v.size());
  st.avg_degree = (st.nodes > 0) ? (static_cast<double>(sum_deg) / static_cast<double>(st.nodes)) : 0.0;

  // Connected components + diameter (unweighted graph).
  {
    st.component_of_node.assign(static_cast<std::size_t>(st.nodes), -1);

    int comp = 0;
    for (int start = 0; start < st.nodes; ++start) {
      if (st.component_of_node[static_cast<std::size_t>(start)] != -1) continue;

      std::queue<int> q;
      st.component_of_node[static_cast<std::size_t>(start)] = comp;
      q.push(start);

      while (!q.empty()) {
        const int u = q.front();
        q.pop();
        for (const int v : adj[static_cast<std::size_t>(u)]) {
          if (st.component_of_node[static_cast<std::size_t>(v)] != -1) continue;
          st.component_of_node[static_cast<std::size_t>(v)] = comp;
          q.push(v);
        }
      }

      ++comp;
    }

    st.components = std::max(1, comp);
    st.connected = (st.components == 1);

    // Diameter: max shortest-path distance within components.
    auto bfs_all_dist = [&](int src, std::vector<int>& d) {
      std::fill(d.begin(), d.end(), -1);
      std::queue<int> qq;
      d[static_cast<std::size_t>(src)] = 0;
      qq.push(src);
      while (!qq.empty()) {
        const int u = qq.front();
        qq.pop();
        for (const int v : adj[static_cast<std::size_t>(u)]) {
          if (d[static_cast<std::size_t>(v)] != -1) continue;
          d[static_cast<std::size_t>(v)] = d[static_cast<std::size_t>(u)] + 1;
          qq.push(v);
        }
      }
    };

    auto bfs_farthest = [&](int src, std::vector<int>& d) {
      bfs_all_dist(src, d);
      int far_node = src;
      int far_dist = 0;
      const int src_comp = st.component_of_node[static_cast<std::size_t>(src)];
      for (int i = 0; i < st.nodes; ++i) {
        if (st.component_of_node[static_cast<std::size_t>(i)] != src_comp) continue;
        const int di = d[static_cast<std::size_t>(i)];
        if (di > far_dist) {
          far_dist = di;
          far_node = i;
        }
      }
      return std::pair<int, int>{far_node, far_dist};
    };

    int diameter = 0;
    const bool exact_diameter = (st.nodes <= kJumpStatsExactNodeCap && st.undirected_edges <= kJumpStatsExactEdgeCap);
    std::vector<int> d(static_cast<std::size_t>(st.nodes), -1);

    if (exact_diameter) {
      for (int src = 0; src < st.nodes; ++src) {
        bfs_all_dist(src, d);
        for (int dd : d) diameter = std::max(diameter, dd);
      }
    } else {
      std::vector<int> seeds;
      seeds.reserve(std::min(st.nodes, kJumpDiameterApproxSeedCount * 2));
      seeds.push_back(0);
      seeds.push_back(st.nodes / 2);
      seeds.push_back(st.nodes - 1);

      // Add one representative per component (up to budget).
      std::vector<char> seen_comp(static_cast<std::size_t>(st.components), 0);
      for (int i = 0; i < st.nodes && static_cast<int>(seeds.size()) < kJumpDiameterApproxSeedCount; ++i) {
        const int cidx = st.component_of_node[static_cast<std::size_t>(i)];
        if (cidx < 0 || cidx >= st.components) continue;
        if (seen_comp[static_cast<std::size_t>(cidx)]) continue;
        seen_comp[static_cast<std::size_t>(cidx)] = 1;
        seeds.push_back(i);
      }

      std::sort(seeds.begin(), seeds.end());
      seeds.erase(std::unique(seeds.begin(), seeds.end()), seeds.end());

      for (int seed : seeds) {
        if (seed < 0 || seed >= st.nodes) continue;
        const auto far1_pair = bfs_farthest(seed, d);
        const int far1 = far1_pair.first;
        const auto far2_pair = bfs_farthest(far1, d);
        const int d2 = far2_pair.second;
        diameter = std::max(diameter, d2);
      }
    }
    st.diameter_hops = std::max(0, diameter);
  }

  // Articulation points ("chokepoints") via DFS lowlink (Tarjan).
  {
    std::vector<int> disc(static_cast<std::size_t>(st.nodes), -1);
    std::vector<int> low(static_cast<std::size_t>(st.nodes), -1);
    std::vector<int> parent(static_cast<std::size_t>(st.nodes), -1);
    std::vector<bool> ap(static_cast<std::size_t>(st.nodes), false);

    int t = 0;
    auto dfs = [&](auto&& self, int u) -> void {
      disc[static_cast<std::size_t>(u)] = low[static_cast<std::size_t>(u)] = t++;
      int children = 0;

      for (const int v : adj[static_cast<std::size_t>(u)]) {
        if (disc[static_cast<std::size_t>(v)] == -1) {
          parent[static_cast<std::size_t>(v)] = u;
          ++children;

          self(self, v);

          low[static_cast<std::size_t>(u)] = std::min(low[static_cast<std::size_t>(u)], low[static_cast<std::size_t>(v)]);

          // Root with 2+ children.
          if (parent[static_cast<std::size_t>(u)] == -1 && children > 1) ap[static_cast<std::size_t>(u)] = true;

          // Non-root: if v can't reach above u.
          if (parent[static_cast<std::size_t>(u)] != -1 &&
              low[static_cast<std::size_t>(v)] >= disc[static_cast<std::size_t>(u)]) {
            ap[static_cast<std::size_t>(u)] = true;
          }
        } else if (v != parent[static_cast<std::size_t>(u)]) {
          low[static_cast<std::size_t>(u)] = std::min(low[static_cast<std::size_t>(u)], disc[static_cast<std::size_t>(v)]);
        }
      }
    };

    for (int i = 0; i < st.nodes; ++i) {
      if (disc[static_cast<std::size_t>(i)] == -1) dfs(dfs, i);
    }

    st.articulation_systems.clear();
    st.articulation_systems.reserve(static_cast<std::size_t>(st.nodes));
    for (int i = 0; i < st.nodes; ++i) {
      if (!ap[static_cast<std::size_t>(i)]) continue;
      if (i < 0 || i >= static_cast<int>(ids.size())) continue;
      st.articulation_systems.push_back(ids[static_cast<std::size_t>(i)]);
    }
    st.articulation_points = static_cast<int>(st.articulation_systems.size());
  }

  return st;
}


struct RegionStats {
  int regions{0};
  int assigned_systems{0};
  int min_systems{0};
  int max_systems{0};
  double avg_systems{0.0};

  // Theme counts, sorted for display.
  std::vector<std::pair<std::string, int>> themes;
};

RegionStats compute_region_stats(const GameState& s) {
  RegionStats out;
  out.regions = static_cast<int>(s.regions.size());

  std::unordered_map<Id, int> counts;
  counts.reserve(s.regions.size() * 2);

  std::unordered_map<std::string, int> theme_counts;
  theme_counts.reserve(s.regions.size() * 2);

  for (const auto& [sid, sys] : s.systems) {
    (void)sid;
    if (sys.region_id == kInvalidId) continue;
    ++out.assigned_systems;
    ++counts[sys.region_id];
  }

  if (!counts.empty()) {
    out.min_systems = std::numeric_limits<int>::max();
    out.max_systems = 0;
    int total = 0;

    for (const auto& [rid, c] : counts) {
      out.min_systems = std::min(out.min_systems, c);
      out.max_systems = std::max(out.max_systems, c);
      total += c;

      if (const auto* reg = nebula4x::find_ptr(s.regions, rid)) {
        if (!reg->theme.empty()) ++theme_counts[reg->theme];
      }
    }

    out.avg_systems = counts.empty() ? 0.0 : (double)total / (double)counts.size();
  } else {
    out.min_systems = 0;
    out.max_systems = 0;
    out.avg_systems = 0.0;
  }

  out.themes.reserve(theme_counts.size());
  for (auto& kv : theme_counts) out.themes.push_back(kv);
  std::sort(out.themes.begin(), out.themes.end(),
            [](const auto& a, const auto& b) { return (a.second != b.second) ? (a.second > b.second) : (a.first < b.first); });
  return out;
}


struct NebulaStats {
  int systems{0};
  double avg_density{0.0};
  double density_std{0.0};
  double min_density{0.0};
  double max_density{0.0};
  int dense_systems{0};
};

NebulaStats compute_nebula_stats(const GameState& s) {
  NebulaStats st;
  st.systems = static_cast<int>(s.systems.size());
  if (st.systems <= 0) return st;

  double sum = 0.0;
  double sum2 = 0.0;
  st.min_density = 1e9;
  st.max_density = 0.0;
  st.dense_systems = 0;

  for (const auto& [_, sys] : s.systems) {
    const double d = std::clamp(sys.nebula_density, 0.0, 1.0);
    sum += d;
    sum2 += d * d;
    st.min_density = std::min(st.min_density, d);
    st.max_density = std::max(st.max_density, d);
    if (d >= 0.50) ++st.dense_systems;
  }

  const double n = static_cast<double>(st.systems);
  st.avg_density = sum / std::max(1e-9, n);
  const double var = std::max(0.0, (sum2 / std::max(1e-9, n)) - st.avg_density * st.avg_density);
  st.density_std = std::sqrt(var);
  if (!std::isfinite(st.min_density)) st.min_density = 0.0;
  if (!std::isfinite(st.max_density)) st.max_density = 0.0;
  return st;
}

struct FrontierStats {
  int anomalies{0};
  int wrecks{0};
  double anomaly_avg_dist_norm{0.0};
  double wreck_avg_dist_norm{0.0};
  double avg_hazard{0.0};
  double inner_region_risk{0.0};
  double outer_region_risk{0.0};
  double risk_gradient{0.0};
};

FrontierStats compute_frontier_stats(const GameState& s) {
  FrontierStats st;
  if (s.systems.empty()) return st;

  const Id home_id = (s.selected_system != kInvalidId && find_ptr(s.systems, s.selected_system) != nullptr)
                         ? s.selected_system
                         : s.systems.begin()->first;
  const auto* home = find_ptr(s.systems, home_id);
  if (!home) return st;
  const Vec2 home_pos = home->galaxy_pos;

  std::unordered_map<Id, double> dist_norm;
  dist_norm.reserve(s.systems.size() * 2);

  double max_d = 1e-6;
  for (const auto& [sid, sys] : s.systems) {
    const double d = (sys.galaxy_pos - home_pos).length();
    max_d = std::max(max_d, d);
    dist_norm[sid] = d;
  }
  for (auto& [sid, d] : dist_norm) {
    (void)sid;
    d = std::clamp(d / max_d, 0.0, 1.0);
  }

  double an_sum = 0.0;
  double hz_sum = 0.0;
  for (const auto& [_, a] : s.anomalies) {
    const auto it = dist_norm.find(a.system_id);
    if (it == dist_norm.end()) continue;
    an_sum += it->second;
    hz_sum += std::max(0.0, a.hazard_chance) * std::max(0.0, a.hazard_damage);
    ++st.anomalies;
  }
  if (st.anomalies > 0) {
    st.anomaly_avg_dist_norm = an_sum / static_cast<double>(st.anomalies);
    st.avg_hazard = hz_sum / static_cast<double>(st.anomalies);
  }

  double wr_sum = 0.0;
  for (const auto& [_, w] : s.wrecks) {
    const auto it = dist_norm.find(w.system_id);
    if (it == dist_norm.end()) continue;
    wr_sum += it->second;
    ++st.wrecks;
  }
  if (st.wrecks > 0) st.wreck_avg_dist_norm = wr_sum / static_cast<double>(st.wrecks);

  double inner_sum = 0.0;
  int inner_n = 0;
  double outer_sum = 0.0;
  int outer_n = 0;
  for (const auto& [sid, sys] : s.systems) {
    const auto it = dist_norm.find(sid);
    if (it == dist_norm.end()) continue;
    const double dn = it->second;
    double risk = 0.20;
    if (const auto* reg = find_ptr(s.regions, sys.region_id)) risk = std::clamp(reg->pirate_risk, 0.0, 1.0);
    if (dn <= 0.45) {
      inner_sum += risk;
      ++inner_n;
    } else if (dn >= 0.70) {
      outer_sum += risk;
      ++outer_n;
    }
  }
  st.inner_region_risk = (inner_n > 0) ? (inner_sum / static_cast<double>(inner_n)) : 0.0;
  st.outer_region_risk = (outer_n > 0) ? (outer_sum / static_cast<double>(outer_n)) : 0.0;
  st.risk_gradient = st.outer_region_risk - st.inner_region_risk;
  return st;
}

struct CandidateMetrics {
  StarPlacementStats ps;
  JumpGraphStats gs;
  RegionStats rs;
  NebulaStats ns;
  FrontierStats fs;
};

CandidateMetrics compute_candidate_metrics(const GameState& s, bool parallel) {
  CandidateMetrics out;
  if (!parallel) {
    out.ps = compute_star_placement_stats(s);
    out.gs = compute_jump_graph_stats(s);
    out.rs = compute_region_stats(s);
    out.ns = compute_nebula_stats(s);
    out.fs = compute_frontier_stats(s);
    return out;
  }

  // Parallelize the heavier graph/spacing/frontier passes on large maps.
  auto ps_future = std::async(std::launch::async, [&s]() { return compute_star_placement_stats(s); });
  auto gs_future = std::async(std::launch::async, [&s]() { return compute_jump_graph_stats(s); });
  auto fs_future = std::async(std::launch::async, [&s]() { return compute_frontier_stats(s); });

  out.rs = compute_region_stats(s);
  out.ns = compute_nebula_stats(s);
  out.ps = ps_future.get();
  out.gs = gs_future.get();
  out.fs = fs_future.get();
  return out;
}


double score_seed_candidate(int objective,
                            const JumpGraphStats& gs,
                            const StarPlacementStats& ps,
                            const NebulaStats& ns,
                            const RegionStats& rs,
                            const FrontierStats& fs) {
  (void)rs;
  if (!gs.connected) return -1e30;

  const double nodes = std::max(1.0, static_cast<double>(gs.nodes));

  // Some normalization helpers.
  const double crossings = static_cast<double>(gs.edge_crossings);
  const double aps = static_cast<double>(gs.articulation_points);
  const double diam = static_cast<double>(gs.diameter_hops);
  const double deg = gs.avg_degree;
  const double nn_min = ps.min_nearest_neighbor;
  const double nn_sigma = ps.nearest_neighbor_std;

  // Heuristics:
  // 0 = Balanced
  // 1 = Readable (few crossings + nice spacing)
  // 2 = Chokepoints (high articulation)
  // 3 = Webby (redundant routes)
  // 4 = Frontier drama (risk/reward gradient toward the rim)
  switch (objective) {
    case 1: {
      // Prefer low crossings and a reasonable minimum nearest-neighbor distance.
      double score = 0.0;
      score += 2.0 * nn_min;
      score -= 1.2 * nn_sigma;
      score -= 3.0 * crossings;
      score -= 0.25 * aps;
      // Slight preference for moderate diameter to keep exploration readable.
      const double target_d = std::clamp(nodes / 6.0, 3.0, 10.0);
      score -= std::fabs(diam - target_d) * 0.6;
      return score;
    }
    case 2: {
      // Chokepoint-friendly networks: lots of articulation points, larger diameter,
      // and not overly webby.
      double score = 0.0;
      score += 3.0 * aps;
      score += 1.2 * diam;
      score -= 0.8 * deg;
      score -= 0.35 * crossings;
      return score;
    }
    case 3: {
      // Webby: redundancy is good, chokepoints are bad.
      double score = 0.0;
      score += 2.0 * deg;
      score -= 2.5 * aps;
      score -= 0.9 * diam;
      score -= 0.15 * crossings;
      return score;
    }
    case 4: {
      double score = 0.0;
      score += 8.0 * fs.risk_gradient;
      score += 4.5 * fs.anomaly_avg_dist_norm;
      score += 3.0 * fs.wreck_avg_dist_norm;
      score += 0.8 * fs.avg_hazard;
      score += 0.25 * aps;
      score -= 0.40 * crossings;
      return score;
    }
    case 0:
    default: {
      // Balanced: moderate diameter, moderate chokepoints, low crossings, and
      // a mid-range nebula coverage.
      const double target_d = std::clamp(nodes / 5.5, 3.0, 12.0);
      const double target_ap = std::clamp(nodes / 10.0, 0.0, 10.0);
      const double target_neb = 0.15;

      double score = 0.0;
      score -= std::fabs(diam - target_d) * 1.0;
      score -= std::fabs(aps - target_ap) * 0.9;
      score -= crossings * 0.9;
      score += deg * 0.25;
      score += nn_min * 0.4;
      score -= std::fabs(ns.avg_density - target_neb) * 4.0;
      return score;
    }
  }
}


void draw_galaxy_preview(const GameState& s, const UIState& ui, const JumpGraphStats& gs) {
  ImVec2 avail = ImGui::GetContentRegionAvail();
  const float h = std::clamp(avail.y, 120.0f, 240.0f);
  const ImVec2 size(avail.x, h);

  if (ImGui::BeginChild("##new_game_galaxy_preview", size, true)) {
    const ImVec2 region = ImGui::GetContentRegionAvail();

    ImGui::InvisibleButton("##galaxy_preview_canvas", region);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 rmin = ImGui::GetItemRectMin();
    const ImVec2 rmax = ImGui::GetItemRectMax();

    // Background.
    const ImU32 bg = ImGui::GetColorU32(ImGuiCol_FrameBg);
    dl->AddRectFilled(rmin, rmax, bg);

    if (s.systems.empty()) {
      ImGui::EndChild();
      return;
    }

    // Bounds.
    double minx = 1e30, maxx = -1e30, miny = 1e30, maxy = -1e30;
    for (const auto& [_, sys] : s.systems) {
      minx = std::min(minx, sys.galaxy_pos.x);
      maxx = std::max(maxx, sys.galaxy_pos.x);
      miny = std::min(miny, sys.galaxy_pos.y);
      maxy = std::max(maxy, sys.galaxy_pos.y);
    }
    const double dx = std::max(1e-6, maxx - minx);
    const double dy = std::max(1e-6, maxy - miny);

    const float pad = 10.0f;
    const float w = std::max(1.0f, region.x - pad * 2.0f);
    const float h2 = std::max(1.0f, region.y - pad * 2.0f);

    const double sx = static_cast<double>(w) / dx;
    const double sy = static_cast<double>(h2) / dy;
    const double scale = std::min(sx, sy);

    const float ox = pad + static_cast<float>((static_cast<double>(w) - dx * scale) * 0.5);
    const float oy = pad + static_cast<float>((static_cast<double>(h2) - dy * scale) * 0.5);

    auto to_screen = [&](const Vec2& gp) -> ImVec2 {
      const float x = rmin.x + ox + static_cast<float>((gp.x - minx) * scale);
      // Flip Y so positive galaxy_pos.y is "up".
      const float y = rmin.y + oy + static_cast<float>((maxy - gp.y) * scale);
      return ImVec2(x, y);
    };

    auto region_col = [&](Id rid, float alpha) -> ImU32 {
      if (rid == kInvalidId) return 0;
      const float h = std::fmod(static_cast<float>((static_cast<std::uint32_t>(rid) * 0.61803398875f)), 1.0f);
      const ImVec4 c = ImColor::HSV(h, 0.55f, 0.95f, alpha);
      return ImGui::ColorConvertFloat4ToU32(c);
    };

    // Component coloring (only really useful if the generator produces a disconnected graph).
    std::unordered_map<Id, int> comp_of;
    if (ui.new_game_preview_color_by_component && gs.components > 1 &&
        gs.node_ids.size() == gs.component_of_node.size()) {
      comp_of.reserve(gs.node_ids.size() * 2 + 8);
      for (std::size_t i = 0; i < gs.node_ids.size(); ++i) {
        comp_of[gs.node_ids[i]] = gs.component_of_node[i];
      }
    }

    std::unordered_set<Id> chokepoints;
    if (ui.new_game_preview_show_chokepoints && !gs.articulation_systems.empty()) {
      chokepoints.reserve(gs.articulation_systems.size() * 2 + 8);
      for (Id id : gs.articulation_systems) chokepoints.insert(id);
    }

    auto comp_col = [&](int comp, float alpha) -> ImU32 {
      const float h = std::fmod(static_cast<float>(comp) * 0.273f, 1.0f);
      const ImVec4 c = ImColor::HSV(h, 0.60f, 0.95f, alpha);
      return ImGui::ColorConvertFloat4ToU32(c);
    };

    // Draw jump connections.
    std::unordered_set<std::uint64_t> drawn;
    drawn.reserve(s.jump_points.size() * 2);

    const ImU32 line_col = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    if (ui.new_game_preview_show_jumps) {
      for (const auto& [_, jp] : s.jump_points) {
        const auto* other = nebula4x::find_ptr(s.jump_points, jp.linked_jump_id);
        if (!other) continue;
        const Id a = jp.system_id;
        const Id b = other->system_id;
        if (a == kInvalidId || b == kInvalidId) continue;
        const auto lo = static_cast<std::uint32_t>(std::min(a, b));
        const auto hi = static_cast<std::uint32_t>(std::max(a, b));
        const std::uint64_t key = (static_cast<std::uint64_t>(lo) << 32) | static_cast<std::uint64_t>(hi);
        if (!drawn.insert(key).second) continue;

        const auto* sys_a = nebula4x::find_ptr(s.systems, a);
        const auto* sys_b = nebula4x::find_ptr(s.systems, b);
        if (!sys_a || !sys_b) continue;

        dl->AddLine(to_screen(sys_a->galaxy_pos), to_screen(sys_b->galaxy_pos), line_col, 1.0f);
      }
    }

    // Draw systems.
    const ImU32 star_col = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 home_col = ImGui::GetColorU32(ImGuiCol_PlotHistogram);
    const ImU32 choke_col = ImGui::GetColorU32(ImGuiCol_PlotLines);

    const int nsys = static_cast<int>(s.systems.size());
    const bool dense_labels = (nsys <= 24);
    const bool label_all = dense_labels && ui.new_game_preview_show_labels;

    struct LabelBox {
      ImVec2 a;
      ImVec2 b;
    };
    std::vector<LabelBox> label_boxes;
    label_boxes.reserve(64);

    auto can_place_label = [&](const LabelBox& box) {
      for (const auto& o : label_boxes) {
        const bool overlap = !(box.b.x < o.a.x || box.a.x > o.b.x || box.b.y < o.a.y || box.a.y > o.b.y);
        if (overlap) return false;
      }
      return true;
    };

    // We draw labels after stars, but we compute them during the star pass to
    // avoid re-transforming coordinates.
    struct PendingLabel {
      ImVec2 p;
      std::string text;
      ImU32 col{0};
    };
    std::vector<PendingLabel> pending_labels;
    pending_labels.reserve(64);

    for (const auto& [id, sys] : s.systems) {
      const ImVec2 p = to_screen(sys.galaxy_pos);
      const float r = (id == s.selected_system) ? 6.0f : 4.0f;
      ImU32 col = (id == s.selected_system) ? home_col : star_col;

      if (!comp_of.empty()) {
        if (auto it = comp_of.find(id); it != comp_of.end()) {
          col = comp_col(it->second, 1.0f);
        }
      }

      if (ui.new_game_preview_show_regions && sys.region_id != kInvalidId) {
        dl->AddCircleFilled(p, r + 6.5f, region_col(sys.region_id, 0.10f));
      }
      const float neb = (float)std::clamp(sys.nebula_density, 0.0, 1.0);
      if (ui.new_game_preview_show_nebula && neb > 0.01f) {
        const float nr = r + 10.0f + neb * 14.0f;
        const ImU32 ncol = ImGui::GetColorU32(ImGuiCol_PlotHistogramHovered, 0.06f + 0.22f * neb);
        dl->AddCircleFilled(p, nr, ncol);
      }
      procgen_gfx::draw_star_glyph(dl, p, r, static_cast<std::uint32_t>(id), col, 1.0f);

      if (ui.new_game_preview_show_chokepoints && !chokepoints.empty() && chokepoints.count(id)) {
        dl->AddCircle(p, r + 9.0f, choke_col, 14, 2.0f);
      }

      // Labels: keep the preview readable by avoiding heavy clutter on larger maps.
      const bool want_label = ui.new_game_preview_show_labels &&
                              (label_all || id == s.selected_system || (ui.new_game_preview_show_chokepoints && chokepoints.count(id)));
      if (want_label) {
        const ImU32 tcol = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        pending_labels.push_back({ImVec2(p.x + 7.0f, p.y - 8.0f), sys.name, tcol});
      }
    }

    // Label pass with simple overlap avoidance.
    for (const auto& pl : pending_labels) {
      const ImVec2 ts = ImGui::CalcTextSize(pl.text.c_str());
      LabelBox box{pl.p, ImVec2(pl.p.x + ts.x, pl.p.y + ts.y)};
      if (!can_place_label(box)) continue;
      label_boxes.push_back(box);
      dl->AddText(pl.p, pl.col, pl.text.c_str());
    }

    // Hover tooltip.
    if (ImGui::IsItemHovered()) {
      const ImVec2 m = ImGui::GetIO().MousePos;
      const float hit_r2 = 8.0f * 8.0f;

      Id best_id = kInvalidId;
      float best_d2 = hit_r2;

      for (const auto& [id, sys] : s.systems) {
        const ImVec2 p = to_screen(sys.galaxy_pos);
        const float dx2 = m.x - p.x;
        const float dy2 = m.y - p.y;
        const float d2 = dx2 * dx2 + dy2 * dy2;
        if (d2 <= best_d2) {
          best_d2 = d2;
          best_id = id;
        }
      }

      if (best_id != kInvalidId) {
        const auto* sys = nebula4x::find_ptr(s.systems, best_id);
        if (sys) {
          ImGui::BeginTooltip();
          ImGui::Text("%s", sys->name.c_str());
          const double neb = std::clamp(sys->nebula_density, 0.0, 1.0);
          if (neb > 0.01) {
            ImGui::TextDisabled("Nebula: %.0f%%", neb * 100.0);
          } else {
            ImGui::TextDisabled("Nebula: none");
          }
          if (sys->region_id != kInvalidId) {
            if (const auto* reg = find_ptr(s.regions, sys->region_id)) {
              ImGui::TextDisabled("Region: %s", reg->name.c_str());
              if (!reg->theme.empty()) ImGui::TextDisabled("Theme: %s", reg->theme.c_str());
            }
          }

          ImGui::Separator();
          ImGui::TextDisabled("Systems: %d", static_cast<int>(s.systems.size()));
          ImGui::TextDisabled("Jump points: %d", static_cast<int>(s.jump_points.size()));
          ImGui::EndTooltip();
        }
      }
    }
  }

  ImGui::EndChild();
}

} // namespace

void draw_new_game_modal(Simulation& sim, UIState& ui) {
  if (!ui.show_new_game_modal) return;

  // Keep the popup open while the flag is set.
  ImGui::OpenPopup("New Game");

  bool open = true;
  if (ImGui::BeginPopupModal("New Game", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
    ui.new_game_scenario = std::clamp(ui.new_game_scenario, kScenarioSol, kScenarioRandom);

    ImGui::Text("Choose scenario");

    if (ImGui::RadioButton("Sol (classic)", ui.new_game_scenario == kScenarioSol)) {
      ui.new_game_scenario = kScenarioSol;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Random galaxy (procedural)", ui.new_game_scenario == kScenarioRandom)) {
      ui.new_game_scenario = kScenarioRandom;
    }

    ImGui::Separator();

    static RandomPreviewCache preview;
    static std::string start_error;

    struct PreviewMetricsCache {
      bool valid{false};
      std::uint32_t seed{0};
      int requested_systems{0};
      int generated_systems{0};
      JumpGraphStats gs;
      StarPlacementStats ps;
      NebulaStats ns;
      RegionStats rs;
      FrontierStats fs;
      double compute_ms{0.0};
    };

    static PreviewMetricsCache preview_metrics;

    struct SeedSearchRuntime {
      bool active{false};
      std::uint64_t cfg_sig{0};

      int objective{0};
      int total_tries{0};
      int tried{0};

      std::uint32_t base_seed{0};
      std::uint32_t best_seed{0};
      double best_score{-1e30};
      bool best_applied{false};
      bool used_proxy{false};
      int proxy_systems{0};
      double avg_candidate_ms{0.0};
      double last_candidate_ms{0.0};
      std::chrono::steady_clock::time_point started_at{};

      // Best candidate metrics (for UI feedback).
      JumpGraphStats best_gs;
      StarPlacementStats best_ps;
      NebulaStats best_ns;
      RegionStats best_rs;
      FrontierStats best_fs;

      std::string last_error;

      void reset() {
        active = false;
        cfg_sig = 0;
        objective = 0;
        total_tries = 0;
        tried = 0;
        base_seed = 0;
        best_seed = 0;
        best_score = -1e30;
        best_applied = false;
        used_proxy = false;
        proxy_systems = 0;
        avg_candidate_ms = 0.0;
        last_candidate_ms = 0.0;
        started_at = std::chrono::steady_clock::time_point{};
        best_gs = JumpGraphStats{};
        best_ps = StarPlacementStats{};
        best_ns = NebulaStats{};
        best_rs = RegionStats{};
        best_fs = FrontierStats{};
        last_error.clear();
      }
    };

    static SeedSearchRuntime seed_search;

    if (ImGui::IsWindowAppearing()) {
      start_error.clear();
      preview_metrics.valid = false;
    }

    if (ui.new_game_scenario == kScenarioSol) {
      ImGui::TextWrapped(
          "A compact starter scenario in the Sol system. Good for learning the UI and testing early ship designs.");

    } else {
      // --- Random scenario settings ---
      ui.new_game_random_num_systems = std::clamp(ui.new_game_random_num_systems, 1, kMaxRandomSystems);

      ImGui::Text("Random galaxy settings");

      // Seed.
      {
        std::uint32_t seed = ui.new_game_random_seed;
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputScalar("Seed", ImGuiDataType_U32, &seed);
        ui.new_game_random_seed = seed;

        ImGui::SameLine();
        if (ImGui::Button("Randomize")) {
          ui.new_game_random_seed = time_seed_u32();
          preview.valid = false;
        }
      }

      // System count.
      {
        int n = ui.new_game_random_num_systems;
        ImGui::SetNextItemWidth(180.0f);
        ImGui::SliderInt("Systems", &n, 1, kMaxRandomSystems);
        ui.new_game_random_num_systems = std::clamp(n, 1, kMaxRandomSystems);
      }

      // Galaxy archetype.
      {
        int shape = std::clamp(ui.new_game_random_galaxy_shape, 0, 5);
        ImGui::SetNextItemWidth(180.0f);
        ImGui::Combo("Galaxy shape", &shape,
                     "Spiral disc\0Uniform disc\0Ring\0Clusters\0Filaments\0Barred spiral\0");
        ui.new_game_random_galaxy_shape = std::clamp(shape, 0, 5);
      }

      // System placement style.
      {
        int ps = std::clamp(ui.new_game_random_placement_style, 0, 1);
        ImGui::SetNextItemWidth(180.0f);
        ImGui::Combo("Placement", &ps, "Classic\0Blue noise\0");
        ui.new_game_random_placement_style = std::clamp(ps, 0, 1);

        if (ui.new_game_random_placement_style == 1) {
          int q = std::clamp(ui.new_game_random_placement_quality, 4, 96);
          ImGui::SetNextItemWidth(180.0f);
          ImGui::SliderInt("Placement quality", &q, 8, 64);
          ui.new_game_random_placement_quality = std::clamp(q, 4, 96);
          ImGui::SameLine();
          ImGui::TextDisabled("candidates");
        }
      }


      // Jump network archetype.
      {
        int style = std::clamp(ui.new_game_random_jump_network_style, 0, 6);
        ImGui::SetNextItemWidth(180.0f);
        ImGui::Combo("Jump network", &style,
                     "Balanced\0Dense web\0Sparse lanes\0Cluster bridges\0Hub & spoke\0Planar proximity\0Subspace rivers\0");
        ui.new_game_random_jump_network_style = std::clamp(style, 0, 6);
      }

      // Jump density (scales how many additional links get added for the chosen archetype).
      {
        float dens = ui.new_game_random_jump_density;
        ImGui::SetNextItemWidth(180.0f);
        ImGui::SliderFloat("Jump density", &dens, 0.0f, 2.0f, "%.2fx");
        ui.new_game_random_jump_density = std::clamp(dens, 0.0f, 2.0f);
      }

      // Macro knobs for progression and economy tuning.
      {
        float abundance = ui.new_game_random_resource_abundance;
        ImGui::SetNextItemWidth(180.0f);
        ImGui::SliderFloat("Resource abundance", &abundance, 0.5f, 2.0f, "%.2fx");
        ui.new_game_random_resource_abundance = std::clamp(abundance, 0.5f, 2.0f);

        float frontier = ui.new_game_random_frontier_intensity;
        ImGui::SetNextItemWidth(180.0f);
        ImGui::SliderFloat("Frontier intensity", &frontier, 0.5f, 2.0f, "%.2fx");
        ui.new_game_random_frontier_intensity = std::clamp(frontier, 0.5f, 2.0f);

        float xeno_early = ui.new_game_random_xeno_spawn_pressure_early;
        ImGui::SetNextItemWidth(180.0f);
        ImGui::SliderFloat("Xeno pressure (early)", &xeno_early, 0.25f, 3.0f, "%.2fx");
        ui.new_game_random_xeno_spawn_pressure_early = std::clamp(xeno_early, 0.25f, 3.0f);

        float xeno_late = ui.new_game_random_xeno_spawn_pressure_late;
        ImGui::SetNextItemWidth(180.0f);
        ImGui::SliderFloat("Xeno pressure (late)", &xeno_late, 0.25f, 3.0f, "%.2fx");
        ui.new_game_random_xeno_spawn_pressure_late = std::clamp(xeno_late, 0.25f, 3.0f);

        ImGui::TextDisabled("Xeno pressure presets");
        if (ImGui::Button("Balanced##xeno_preset")) {
          ui.new_game_random_xeno_spawn_pressure_early = 1.00f;
          ui.new_game_random_xeno_spawn_pressure_late = 1.00f;
          preview.valid = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Early surge##xeno_preset")) {
          ui.new_game_random_xeno_spawn_pressure_early = 1.70f;
          ui.new_game_random_xeno_spawn_pressure_late = 0.80f;
          preview.valid = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Late surge##xeno_preset")) {
          ui.new_game_random_xeno_spawn_pressure_early = 0.80f;
          ui.new_game_random_xeno_spawn_pressure_late = 1.70f;
          preview.valid = false;
        }
      }

      {
        ImGui::TextDisabled("Quick presets");
        if (ImGui::Button("Frontier Rush")) {
          ui.new_game_random_jump_network_style = 2; // Sparse lanes
          ui.new_game_random_jump_density = 0.90f;
          ui.new_game_random_resource_abundance = 1.10f;
          ui.new_game_random_frontier_intensity = 1.80f;
          ui.new_game_random_xeno_spawn_pressure_early = 0.90f;
          ui.new_game_random_xeno_spawn_pressure_late = 1.45f;
          ui.new_game_random_enable_pirates = true;
          ui.new_game_random_pirate_strength = 1.35f;
          ui.new_game_random_enable_regions = true;
          preview.valid = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Core Stability")) {
          ui.new_game_random_jump_network_style = 1; // Dense web
          ui.new_game_random_jump_density = 1.20f;
          ui.new_game_random_resource_abundance = 1.00f;
          ui.new_game_random_frontier_intensity = 0.70f;
          ui.new_game_random_xeno_spawn_pressure_early = 0.75f;
          ui.new_game_random_xeno_spawn_pressure_late = 0.95f;
          ui.new_game_random_enable_pirates = true;
          ui.new_game_random_pirate_strength = 0.80f;
          ui.new_game_random_ensure_clear_home = true;
          preview.valid = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Relic Hunt")) {
          ui.new_game_random_galaxy_shape = 4; // Filaments
          ui.new_game_random_jump_network_style = 6; // Subspace rivers
          ui.new_game_random_jump_density = 1.05f;
          ui.new_game_random_resource_abundance = 1.35f;
          ui.new_game_random_frontier_intensity = 1.45f;
          ui.new_game_random_xeno_spawn_pressure_early = 1.10f;
          ui.new_game_random_xeno_spawn_pressure_late = 2.00f;
          ui.new_game_random_enable_regions = true;
          ui.new_game_random_enable_pirates = true;
          ui.new_game_random_pirate_strength = 1.10f;
          preview.valid = false;
        }
      }

      // Additional AI empires (besides the player and pirates).
      {
        int ai = ui.new_game_random_ai_empires;
        ImGui::SetNextItemWidth(180.0f);
        ImGui::SliderInt("AI empires", &ai, -1, 12);
        ui.new_game_random_ai_empires = std::clamp(ai, -1, 12);
        if (ui.new_game_random_ai_empires == -1) {
          ImGui::SameLine();
          ImGui::TextDisabled("Auto");
        }
      }

      // Pirates.
      {
        ImGui::Checkbox("Enable pirates", &ui.new_game_random_enable_pirates);
        if (ui.new_game_random_enable_pirates) {
          float strength = ui.new_game_random_pirate_strength;
          ImGui::SetNextItemWidth(180.0f);
          ImGui::SliderFloat("Pirate strength", &strength, 0.5f, 3.0f, "%.2fx");
          ui.new_game_random_pirate_strength = std::clamp(strength, 0.0f, 5.0f);
        }
      }

      // Galaxy regions / sectors.
      {
        ImGui::Checkbox("Enable regions (sectors)", &ui.new_game_random_enable_regions);
        if (ui.new_game_random_enable_regions) {
          int regions = ui.new_game_random_num_regions;
          ImGui::SetNextItemWidth(180.0f);
          ImGui::SliderInt("Regions", &regions, -1, 12);
          ui.new_game_random_num_regions = std::clamp(regions, -1, 12);
          if (ui.new_game_random_num_regions == -1) {
            ImGui::SameLine();
            ImGui::TextDisabled("Auto");
          }
        }
      }

      // Independent neutral outposts.
      {
        ImGui::Checkbox("Enable independents (neutral outposts)", &ui.new_game_random_enable_independents);
        if (ui.new_game_random_enable_independents) {
          int n = ui.new_game_random_num_independent_outposts;
          ImGui::SetNextItemWidth(180.0f);
          ImGui::SliderInt("Independent outposts", &n, -1, 64);
          ui.new_game_random_num_independent_outposts = std::clamp(n, -1, 64);
          if (ui.new_game_random_num_independent_outposts == -1) {
            ImGui::SameLine();
            ImGui::TextDisabled("Auto");
          }
        }
      }

      // Keep the starting system readable by clamping nebula density.
      {
        ImGui::Checkbox("Ensure clear home system", &ui.new_game_random_ensure_clear_home);
      }

      // Build and sanitize the generator config.
      const nebula4x::RandomScenarioConfig cfg = random_config_from_ui(ui);

      // --- Seed explorer ---
      const std::uint64_t cfg_sig = config_signature_no_seed(cfg);
      if (seed_search.active) {
        const int ui_obj = std::clamp(ui.new_game_seed_search_objective, 0, 4);
        const int ui_tries = std::clamp(ui.new_game_seed_search_tries, 1, 2000);
        if (seed_search.cfg_sig != cfg_sig ||
            seed_search.base_seed != ui.new_game_random_seed ||
            seed_search.objective != ui_obj ||
            seed_search.total_tries != ui_tries) {
          seed_search.reset();
          seed_search.last_error = "Seed explorer canceled: settings changed.";
        }
      }

      if (ImGui::CollapsingHeader("Seed explorer")) {
        static const char* kObjectives = "Balanced\0Readable\0Chokepoints\0Webby\0Frontier drama\0";
        int obj = std::clamp(ui.new_game_seed_search_objective, 0, 4);
        ImGui::SetNextItemWidth(180.0f);
        ImGui::Combo("Objective", &obj, kObjectives);
        ui.new_game_seed_search_objective = std::clamp(obj, 0, 4);

        int tries = std::clamp(ui.new_game_seed_search_tries, 1, 2000);
        ImGui::SetNextItemWidth(180.0f);
        ImGui::SliderInt("Tries", &tries, 1, 2000);
        ui.new_game_seed_search_tries = std::clamp(tries, 1, 2000);

        int spf = std::clamp(ui.new_game_seed_search_steps_per_frame, 1, 200);
        ImGui::SetNextItemWidth(180.0f);
        ImGui::SliderInt("Steps per frame", &spf, 1, 200);
        ui.new_game_seed_search_steps_per_frame = std::clamp(spf, 1, 200);

        if (!seed_search.active) {
          if (ImGui::Button("Search seeds")) {
            seed_search.reset();
            seed_search.active = true;
            seed_search.cfg_sig = cfg_sig;
            seed_search.objective = ui.new_game_seed_search_objective;
            seed_search.total_tries = ui.new_game_seed_search_tries;
            seed_search.tried = 0;
            seed_search.base_seed = ui.new_game_random_seed;
            seed_search.best_seed = ui.new_game_random_seed;
            seed_search.best_score = -1e30;
            seed_search.best_applied = false;
            seed_search.started_at = std::chrono::steady_clock::now();
            seed_search.last_error.clear();
          }
        } else {
          if (ImGui::Button("Cancel search")) {
            seed_search.reset();
          }
          ImGui::SameLine();
          ImGui::TextDisabled("%d / %d", seed_search.tried, seed_search.total_tries);
          const float frac = (seed_search.total_tries > 0)
                                 ? (float)seed_search.tried / (float)seed_search.total_tries
                                 : 0.0f;
          ImGui::ProgressBar(std::clamp(frac, 0.0f, 1.0f), ImVec2(-1.0f, 0.0f));
        }

        if (!seed_search.last_error.empty()) {
          ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "%s", seed_search.last_error.c_str());
        }

        if (seed_search.tried > 0) {
          ImGui::TextDisabled("Timing: last %.1f ms  avg %.1f ms/candidate",
                              seed_search.last_candidate_ms,
                              seed_search.avg_candidate_ms);
          if (seed_search.used_proxy && seed_search.proxy_systems > 0) {
            ImGui::TextDisabled("Search proxy mode: scoring on %d systems for responsiveness.", seed_search.proxy_systems);
          }
          ImGui::TextDisabled("Best seed: %u  score: %.2f", seed_search.best_seed, seed_search.best_score);
          ImGui::TextDisabled("Best graph: deg %.2f  dia %d  ap %d  xings %d",
                              seed_search.best_gs.avg_degree,
                              seed_search.best_gs.diameter_hops,
                              seed_search.best_gs.articulation_points,
                              seed_search.best_gs.edge_crossings);
          ImGui::TextDisabled("Best spacing: min NN %.2f  \xcf\x83 %.2f",
                              seed_search.best_ps.min_nearest_neighbor,
                              seed_search.best_ps.nearest_neighbor_std);
          ImGui::TextDisabled("Best nebula: avg %.0f%%  dense %d/%d",
                              seed_search.best_ns.avg_density * 100.0,
                              seed_search.best_ns.dense_systems,
                              seed_search.best_ns.systems);
          ImGui::TextDisabled("Best frontier: anomaly rim %.0f%%  wreck rim %.0f%%  risk \xce\x94 %.2f",
                              seed_search.best_fs.anomaly_avg_dist_norm * 100.0,
                              seed_search.best_fs.wreck_avg_dist_norm * 100.0,
                              seed_search.best_fs.risk_gradient);
        }
      }

      // Run the seed search incrementally (prevents the UI from freezing when
      // tries is high).
      if (seed_search.active) {
        const auto frame_start = std::chrono::steady_clock::now();
        const auto frame_budget = std::chrono::duration<double, std::milli>(kSeedSearchFrameBudgetMs);

        const double session_budget_ms =
            std::clamp(kSeedSearchSessionBudgetBaseMs +
                           static_cast<double>(seed_search.total_tries) * kSeedSearchSessionBudgetPerTryMs +
                           static_cast<double>(cfg.num_systems) * kSeedSearchSessionBudgetPerSystemMs,
                       2500.0, 60000.0);
        if (seed_search.started_at == std::chrono::steady_clock::time_point{}) {
          seed_search.started_at = frame_start;
        }

        auto apply_best_seed_once = [&]() {
          if (seed_search.best_applied) return;
          if (seed_search.tried <= 0) return;
          seed_search.best_applied = true;
          ui.new_game_random_seed = seed_search.best_seed;
          preview.valid = false;
          preview_metrics.valid = false;
          nebula4x::log::info(std::string("Seed explorer: selected seed ") + std::to_string(seed_search.best_seed));
        };

        const int user_steps = std::clamp(ui.new_game_seed_search_steps_per_frame, 1, 200);
        int steps = user_steps;
        if (seed_search.avg_candidate_ms > 0.1) {
          const int budget_steps = static_cast<int>(std::floor(kSeedSearchFrameBudgetMs / seed_search.avg_candidate_ms));
          steps = std::clamp(budget_steps, 1, user_steps);
        }

        for (int step = 0; step < steps && seed_search.active && seed_search.tried < seed_search.total_tries; ++step) {
          const auto now = std::chrono::steady_clock::now();
          if ((now - frame_start) >= frame_budget) break;

          const double session_elapsed_ms = elapsed_ms(seed_search.started_at, now);
          if (session_elapsed_ms >= session_budget_ms) {
            seed_search.active = false;
            seed_search.last_error =
                "Seed explorer stopped after reaching time budget (" +
                std::to_string(static_cast<int>(std::llround(session_budget_ms))) + " ms).";
            break;
          }

          nebula4x::RandomScenarioConfig probe = cfg;
          const std::uint64_t i = static_cast<std::uint64_t>(seed_search.tried);
          const std::uint32_t cand_seed = (seed_search.tried == 0) ? seed_search.base_seed : mix_seed(seed_search.base_seed, i);
          probe.seed = cand_seed;

          if (probe.num_systems >= kSeedSearchProxyActivateAtSystems && seed_search.total_tries >= 96) {
            if (probe.num_systems > kSeedSearchProxySystems) {
              probe.num_systems = kSeedSearchProxySystems;
              seed_search.used_proxy = true;
              seed_search.proxy_systems = probe.num_systems;
            }
          }

          const auto t0 = std::chrono::steady_clock::now();
          try {
            const GameState st = nebula4x::make_random_scenario(probe);
            const bool parallel_metrics =
                static_cast<int>(st.systems.size()) >= kMetricParallelMinSystems && seed_search.total_tries <= 256;
            const CandidateMetrics m = compute_candidate_metrics(st, parallel_metrics);
            const double score = score_seed_candidate(seed_search.objective, m.gs, m.ps, m.ns, m.rs, m.fs);
            if (score > seed_search.best_score) {
              seed_search.best_score = score;
              seed_search.best_seed = cand_seed;
              seed_search.best_ps = m.ps;
              seed_search.best_gs = m.gs;
              seed_search.best_rs = m.rs;
              seed_search.best_ns = m.ns;
              seed_search.best_fs = m.fs;
            }
          } catch (const std::exception& e) {
            seed_search.last_error = e.what();
          }
          const auto t1 = std::chrono::steady_clock::now();

          seed_search.last_candidate_ms = elapsed_ms(t0, t1);
          if (seed_search.avg_candidate_ms <= 0.0) {
            seed_search.avg_candidate_ms = seed_search.last_candidate_ms;
          } else {
            seed_search.avg_candidate_ms = seed_search.avg_candidate_ms * 0.85 + seed_search.last_candidate_ms * 0.15;
          }

          ++seed_search.tried;

          if (seed_search.last_candidate_ms > kSeedSearchPerCandidateHardMs) {
            seed_search.active = false;
            seed_search.last_error =
                "Seed explorer stopped: single candidate exceeded time budget (" +
                std::to_string(static_cast<int>(std::llround(seed_search.last_candidate_ms))) + " ms).";
            break;
          }
        }

        if (seed_search.tried >= seed_search.total_tries) {
          seed_search.active = false;
          seed_search.cfg_sig = 0;

          // If proxy scoring was used for speed, refresh best metrics with one full-fidelity pass.
          if (seed_search.used_proxy) {
            try {
              nebula4x::RandomScenarioConfig full_probe = cfg;
              full_probe.seed = seed_search.best_seed;
              const GameState st = nebula4x::make_random_scenario(full_probe);
              const bool parallel_metrics = static_cast<int>(st.systems.size()) >= kMetricParallelMinSystems;
              const CandidateMetrics m = compute_candidate_metrics(st, parallel_metrics);
              seed_search.best_ps = m.ps;
              seed_search.best_gs = m.gs;
              seed_search.best_rs = m.rs;
              seed_search.best_ns = m.ns;
              seed_search.best_fs = m.fs;
              seed_search.best_score = score_seed_candidate(seed_search.objective, m.gs, m.ps, m.ns, m.rs, m.fs);
            } catch (const std::exception& e) {
              if (seed_search.last_error.empty()) seed_search.last_error = e.what();
            }
          }

          apply_best_seed_once();
        } else if (!seed_search.active) {
          seed_search.cfg_sig = 0;
          apply_best_seed_once();
        }
      }

      const bool manual = ImGui::Button("Generate preview");
      if (cfg.num_systems > kAutoPreviewProxySystemCap) {
        ImGui::SameLine();
        ImGui::TextDisabled("Auto preview uses %d-system fast mode; button forces full %d-system preview.",
                            kAutoPreviewProxySystemCap, cfg.num_systems);
      }

      // Auto-preview when the user isn't actively editing inputs.
      const int strength_pct = static_cast<int>(std::llround(cfg.pirate_strength * 100.0));
      const int placement_style_i = std::clamp(ui.new_game_random_placement_style, 0, 1);
      const int placement_quality_i = std::clamp(ui.new_game_random_placement_quality, 4, 96);
      const int jump_style_i = std::clamp(ui.new_game_random_jump_network_style, 0, 6);
      const int jump_density_pct = static_cast<int>(std::llround(cfg.jump_density * 100.0));
      const int resource_abundance_pct = static_cast<int>(std::llround(cfg.resource_abundance * 100.0));
      const int frontier_intensity_pct = static_cast<int>(std::llround(cfg.frontier_intensity * 100.0));
      const int xeno_spawn_pressure_early_pct = static_cast<int>(std::llround(cfg.xenoarchaeology_spawn_pressure_early * 100.0));
      const int xeno_spawn_pressure_late_pct = static_cast<int>(std::llround(cfg.xenoarchaeology_spawn_pressure_late * 100.0));
      const bool config_changed = (!preview.valid) || preview.seed != cfg.seed || preview.num_systems != cfg.num_systems ||
                                  preview.galaxy_shape != static_cast<int>(cfg.galaxy_shape) ||
                                  preview.placement_style != placement_style_i ||
                                  preview.placement_quality != placement_quality_i ||
                                  preview.jump_style != jump_style_i || preview.jump_density_pct != jump_density_pct ||
                                  preview.resource_abundance_pct != resource_abundance_pct ||
                                  preview.frontier_intensity_pct != frontier_intensity_pct ||
                                  preview.xeno_spawn_pressure_early_pct != xeno_spawn_pressure_early_pct ||
                                  preview.xeno_spawn_pressure_late_pct != xeno_spawn_pressure_late_pct ||
                                  preview.ai_empires != cfg.num_ai_empires ||
                                  preview.enable_regions != cfg.enable_regions || preview.num_regions != cfg.num_regions ||
                                  preview.enable_pirates != cfg.enable_pirates ||
                                  preview.pirate_strength_pct != strength_pct ||
                                  preview.enable_independents != cfg.enable_independents ||
                                  preview.num_independent_outposts != cfg.num_independent_outposts ||
                                  preview.ensure_clear_home != cfg.ensure_clear_home;

      const bool auto_trigger = (!preview.valid) && !ImGui::IsAnyItemActive();
      if (manual || auto_trigger || config_changed) {
        // Debounce: only regenerate when inputs aren't active, unless explicitly requested.
        if (manual || !ImGui::IsAnyItemActive()) {
          ensure_preview(preview, cfg, manual);
          preview_metrics.valid = false;
        }
      }

      if (!preview.error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "Preview failed: %s", preview.error.c_str());
      }

      if (preview.valid) {
        const GameState& s = preview.state;

        const bool metrics_stale = !preview_metrics.valid ||
                                   preview_metrics.seed != preview.seed ||
                                   preview_metrics.requested_systems != preview.num_systems ||
                                   preview_metrics.generated_systems != preview.generated_systems;
        if (metrics_stale) {
          const auto m0 = std::chrono::steady_clock::now();
          const bool parallel_metrics = static_cast<int>(s.systems.size()) >= kMetricParallelMinSystems;
          const CandidateMetrics m = compute_candidate_metrics(s, parallel_metrics);
          const auto m1 = std::chrono::steady_clock::now();
          preview_metrics.valid = true;
          preview_metrics.seed = preview.seed;
          preview_metrics.requested_systems = preview.num_systems;
          preview_metrics.generated_systems = preview.generated_systems;
          preview_metrics.ps = m.ps;
          preview_metrics.gs = m.gs;
          preview_metrics.rs = m.rs;
          preview_metrics.ns = m.ns;
          preview_metrics.fs = m.fs;
          preview_metrics.compute_ms = elapsed_ms(m0, m1);
        }

        ImGui::Separator();
        ImGui::Text("Preview");
        ImGui::TextDisabled("Systems: %d", static_cast<int>(s.systems.size()));
        ImGui::SameLine();
        ImGui::TextDisabled("Bodies: %d", static_cast<int>(s.bodies.size()));
        ImGui::SameLine();
        ImGui::TextDisabled("Jump points: %d", static_cast<int>(s.jump_points.size()));
        ImGui::TextDisabled("Colonies: %d", static_cast<int>(s.colonies.size()));
        ImGui::SameLine();
        ImGui::TextDisabled("Ships: %d", static_cast<int>(s.ships.size()));
        ImGui::SameLine();
        ImGui::TextDisabled("Factions: %d", static_cast<int>(s.factions.size()));
        if (preview.used_fast_proxy && preview.num_systems > preview.generated_systems) {
          ImGui::TextDisabled("Fast proxy preview: generated %d of requested %d systems (full on manual Generate).",
                              preview.generated_systems, preview.num_systems);
        }
        ImGui::TextDisabled("Preview generation: %.0f ms  metrics: %.0f ms",
                            preview.generation_ms,
                            preview_metrics.compute_ms);

        static const char* kJumpNames[] = {"Balanced", "Dense web", "Sparse lanes", "Cluster bridges", "Hub & spoke",
                                           "Planar proximity", "Subspace rivers"};
        static const char* kPlaceNames[] = {"Classic", "Blue noise"};
        const StarPlacementStats& ps = preview_metrics.ps;
        const JumpGraphStats& gs = preview_metrics.gs;
        const NebulaStats& ns = preview_metrics.ns;
        const FrontierStats& fs = preview_metrics.fs;

        ImGui::TextDisabled("Placement: %s", kPlaceNames[std::clamp(placement_style_i, 0, 1)]);
        if (placement_style_i == 1) {
          ImGui::SameLine();
          ImGui::TextDisabled("Q: %d", placement_quality_i);
        }
        if (ps.nodes > 1) {
          ImGui::TextDisabled("Nearest neighbor: min %.2f u  avg %.2f u  \xcf\x83 %.2f u",
                              ps.min_nearest_neighbor, ps.avg_nearest_neighbor, ps.nearest_neighbor_std);
        }

        ImGui::TextDisabled("Network: %s  Density: %.2fx", kJumpNames[std::clamp(jump_style_i, 0, 6)], cfg.jump_density);
        ImGui::SameLine();
        ImGui::TextDisabled("Resources: %.2fx  Frontier: %.2fx  Xeno E/L: %.2fx / %.2fx",
                            cfg.resource_abundance,
                            cfg.frontier_intensity,
                            cfg.xenoarchaeology_spawn_pressure_early,
                            cfg.xenoarchaeology_spawn_pressure_late);
        ImGui::TextDisabled("Edges: %d", gs.undirected_edges);
        ImGui::SameLine();
        ImGui::TextDisabled("Avg deg: %.2f", gs.avg_degree);
        ImGui::SameLine();
        ImGui::TextDisabled("Avg len: %.2f u", gs.avg_edge_length);
        ImGui::SameLine();
        ImGui::TextDisabled("Crossings: %d", gs.edge_crossings);
        ImGui::TextDisabled("Diameter: %d", gs.diameter_hops);
        ImGui::SameLine();
        ImGui::TextDisabled("Chokepoints: %d", gs.articulation_points);

        ImGui::TextDisabled("Nebula: avg %.0f%%  \xcf\x83 %.0f%%  dense %d/%d",
                            ns.avg_density * 100.0,
                            ns.density_std * 100.0,
                            ns.dense_systems,
                            ns.systems);
        ImGui::TextDisabled("Frontier: anomaly rim %.0f%%  wreck rim %.0f%%  avg hazard %.2f",
                            fs.anomaly_avg_dist_norm * 100.0,
                            fs.wreck_avg_dist_norm * 100.0,
                            fs.avg_hazard);
        ImGui::TextDisabled("Risk gradient: inner %.0f%%  outer %.0f%%  \xce\x94 %.0f%%",
                            fs.inner_region_risk * 100.0,
                            fs.outer_region_risk * 100.0,
                            fs.risk_gradient * 100.0);

        const RegionStats& rs = preview_metrics.rs;
        if (rs.regions > 0) {
          ImGui::TextDisabled("Regions: %d  size %d-%d (avg %.1f)", rs.regions, rs.min_systems, rs.max_systems, rs.avg_systems);
          if (!rs.themes.empty()) {
            std::string themes;
            const std::size_t n = std::min<std::size_t>(3, rs.themes.size());
            for (std::size_t i = 0; i < n; ++i) {
              if (i) themes += ", ";
              themes += rs.themes[i].first + " x" + std::to_string(rs.themes[i].second);
            }
            if (rs.themes.size() > n) themes += ", ...";
            ImGui::TextDisabled("Themes: %s", themes.c_str());
          }
        } else {
          ImGui::TextDisabled("Regions: disabled");
        }

        if (!gs.connected) {
          ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f),
                             "WARNING: jump network is disconnected (%d components)",
                             gs.components);
        }

        ImGui::Separator();
        ImGui::TextDisabled("Preview overlay");
        ImGui::Checkbox("Jumps", &ui.new_game_preview_show_jumps);
        ImGui::SameLine();
        ImGui::Checkbox("Labels", &ui.new_game_preview_show_labels);
        ImGui::SameLine();
        ImGui::Checkbox("Regions", &ui.new_game_preview_show_regions);
        ImGui::SameLine();
        ImGui::Checkbox("Nebula", &ui.new_game_preview_show_nebula);

        ImGui::Checkbox("Chokepoints", &ui.new_game_preview_show_chokepoints);
        ImGui::SameLine();
        ImGui::Checkbox("Color by component", &ui.new_game_preview_color_by_component);

        draw_galaxy_preview(s, ui, gs);
      }
    }

    ImGui::Separator();

    // Buttons.
    const float bw = 140.0f;
    if (!start_error.empty()) {
      ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "Start failed: %s", start_error.c_str());
    }
    if (ImGui::Button("Start", ImVec2(bw, 0.0f))) {
      start_error.clear();
      bool started = false;

      try {
        if (ui.new_game_scenario == kScenarioSol) {
          sim.new_game();
          ui.request_map_tab = MapTab::System;
          nebula4x::log::info("New game: Sol scenario");
        } else {
          const nebula4x::RandomScenarioConfig cfg = random_config_from_ui(ui);
          sim.load_game(nebula4x::make_random_scenario(cfg));
          ui.request_map_tab = MapTab::Galaxy;
          nebula4x::log::info("New game: random galaxy (seed=" + std::to_string(cfg.seed) +
                             ", systems=" + std::to_string(cfg.num_systems) +
                             ", ai=" + std::to_string(cfg.num_ai_empires) +
                             ", jump=" + std::to_string(static_cast<int>(cfg.jump_network_style)) +
                             ", density=" + std::to_string(cfg.jump_density) +
                             ", resources=" + std::to_string(cfg.resource_abundance) +
                             ", frontier=" + std::to_string(cfg.frontier_intensity) +
                             ", pirates=" + std::string(cfg.enable_pirates ? "on" : "off") + ")");
        }
        started = true;
      } catch (const std::exception& e) {
        start_error = e.what();
      } catch (...) {
        start_error = "unknown error while creating new game";
      }

      if (!start_error.empty()) {
        nebula4x::log::error("New game start failed: " + start_error);
      }

      if (started) {
        ui.show_new_game_modal = false;
        ImGui::CloseCurrentPopup();
      }
    }

    ImGui::SameLine();

    if (ImGui::Button("Cancel", ImVec2(bw, 0.0f))) {
      ui.show_new_game_modal = false;
      ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
  }

  if (!open) {
    ui.show_new_game_modal = false;
  }
}

} // namespace nebula4x::ui
