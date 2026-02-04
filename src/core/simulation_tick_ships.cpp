#include "nebula4x/core/simulation.h"

#include "nebula4x/core/colony_profiles.h"

#include "nebula4x/core/contact_prediction.h"
#include "nebula4x/core/intercept.h"

#include "nebula4x/core/fleet_formation.h"

#include "simulation_internal.h"
#include "simulation_sensors.h"
#include "simulation_procgen.h"

#include "nebula4x/core/procgen_obscure.h"
#include "nebula4x/core/procgen_jump_phenomena.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "nebula4x/util/log.h"
#include "nebula4x/util/trace_events.h"

namespace nebula4x {
namespace {
using sim_internal::mkm_per_day_from_speed;
using sim_internal::push_unique;
using sim_internal::sorted_keys;
using sim_internal::compute_power_allocation;
using sim_internal::strongest_active_treaty_between;
using sim_procgen::splitmix64;
using sim_procgen::u01_from_u64;
using sim_procgen::HashRng;
using sim_procgen::pick_site_position_mkm;
using sim_procgen::generate_mineral_bundle;
using sim_procgen::pick_unlock_component_id;


inline double dot(const Vec2& a, const Vec2& b) { return a.x * b.x + a.y * b.y; }

inline Vec2 rotate_vec2(const Vec2& v, double ang_rad) {
  const double c = std::cos(ang_rad);
  const double s = std::sin(ang_rad);
  return Vec2{v.x * c - v.y * s, v.x * s + v.y * c};
}

inline double deg_to_rad(double deg) { return deg * (3.14159265358979323846 / 180.0); }

inline double tau() { return 6.28318530717958647692; }

// Deterministic lost-contact search: generate a low-discrepancy sequence of
// offsets in a disk using a Fibonacci / golden-angle spiral.
//
// waypoint_index:
//  - 0 => center (0,0)
//  - 1.. => spiral samples outward until the radius is filled.
static double contact_search_seed_angle_rad(Id ship_id, Id target_id) {
  std::uint64_t seed = 0x9e3779b97f4a7c15ULL;
  seed ^= static_cast<std::uint64_t>(ship_id) * 0xD6E8FEB86659FD93ULL;
  seed ^= static_cast<std::uint64_t>(target_id) * 0x94D049BB133111EBULL;
  seed = splitmix64(seed);
  const double u = u01_from_u64(seed);
  return u * tau();
}

static Vec2 contact_search_spiral_offset_mkm(int waypoint_index,
                                             int pattern_points,
                                             double radius_mkm,
                                             double seed_angle_rad) {
  if (waypoint_index <= 0) return Vec2{0.0, 0.0};
  if (!(radius_mkm > 1e-9) || !std::isfinite(radius_mkm)) return Vec2{0.0, 0.0};

  const int n = std::max(1, pattern_points);
  const int i = waypoint_index - 1;

  // Golden angle in radians: pi * (3 - sqrt(5)).
  const double golden_angle = 2.39996322972865332223;

  // Fill a disk with roughly uniform area density by using r ~ sqrt(t).
  double t = (static_cast<double>(i) + 0.5) / static_cast<double>(n);
  if (t > 1.0) t = 1.0;
  if (t < 0.0) t = 0.0;
  const double r = radius_mkm * std::sqrt(t);

  const double ang = seed_angle_rad + golden_angle * static_cast<double>(i);
  Vec2 off{std::cos(ang) * r, std::sin(ang) * r};
  if (!std::isfinite(off.x) || !std::isfinite(off.y)) return Vec2{0.0, 0.0};
  return off;
}

// --- Procedural exploration leads (anomaly chains) ---------------------------------
//
// The base game already supports anomalies with rewards and hazards. This layer
// adds *procedural follow-up leads* that can be generated when an anomaly is
// resolved, creating lightweight exploration arcs:
//   - star charts that reveal a short jump-route to a new system,
//   - signal traces that spawn a new anomaly site elsewhere,
//   - hidden caches that spawn a salvageable wreck.
//
// These are intentionally "low UI" (events + journal) and do not require a
// dedicated quest screen.

enum class LeadKind : std::uint8_t {
  None = 0,
  StarChart = 1,
  FollowUpAnomaly = 2,
  HiddenCache = 3,
};

struct LeadOutcome {
  LeadKind kind{LeadKind::None};
  Id target_system_id{kInvalidId};
  Id spawned_anomaly_id{kInvalidId};
  Id spawned_wreck_id{kInvalidId};
  int hops{0};
  bool revealed_route{false};     // discovery/survey lists were updated
  bool revealed_new_system{false}; // target system was previously undiscovered
};

static std::unordered_map<Id, int> compute_system_hops(const GameState& s, Id start_system_id) {
  std::unordered_map<Id, int> dist;
  if (start_system_id == kInvalidId) return dist;
  if (!find_ptr(s.systems, start_system_id)) return dist;

  // Build a stable system adjacency list from jump links.
  std::unordered_map<Id, std::vector<Id>> adj;
  adj.reserve(s.systems.size() * 2);

  for (Id jid : sorted_keys(s.jump_points)) {
    const auto* jp = find_ptr(s.jump_points, jid);
    if (!jp) continue;
    if (jp->linked_jump_id == kInvalidId) continue;
    const auto* lnk = find_ptr(s.jump_points, jp->linked_jump_id);
    if (!lnk) continue;
    const Id a = jp->system_id;
    const Id b = lnk->system_id;
    if (a == kInvalidId || b == kInvalidId || a == b) continue;
    adj[a].push_back(b);
    adj[b].push_back(a);
  }

  for (auto& [_, v] : adj) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
  }

  std::vector<Id> q;
  q.reserve(s.systems.size());
  dist[start_system_id] = 0;
  q.push_back(start_system_id);

  for (std::size_t i = 0; i < q.size(); ++i) {
    const Id cur = q[i];
    const int d = dist[cur];
    auto it = adj.find(cur);
    if (it == adj.end()) continue;
    for (Id nxt : it->second) {
      if (dist.find(nxt) != dist.end()) continue;
      dist[nxt] = d + 1;
      q.push_back(nxt);
    }
  }

  return dist;
}

static Id pick_weighted_system(HashRng& rng,
                               const std::vector<Id>& candidates,
                               const GameState& s,
                               Id origin_system_id,
                               const std::unordered_map<Id, int>& hops,
                               double max_dist,
                               LeadKind kind) {
  const auto* origin = find_ptr(s.systems, origin_system_id);
  if (!origin) return kInvalidId;

  double total_w = 0.0;
  std::vector<double> weights;
  weights.reserve(candidates.size());

  for (Id sid : candidates) {
    const auto* sys = find_ptr(s.systems, sid);
    if (!sys) {
      weights.push_back(0.0);
      continue;
    }
    const auto* reg = (sys->region_id != kInvalidId) ? find_ptr(s.regions, sys->region_id) : nullptr;
    const double ruins = reg ? std::clamp(reg->ruins_density, 0.0, 1.0) : 0.0;
    const double pirate = reg ? std::clamp(reg->pirate_risk, 0.0, 1.0) : 0.0;
    const double salvage_mult = reg ? std::max(0.0, reg->salvage_richness_mult) : 1.0;

    const double d = (sys->galaxy_pos - origin->galaxy_pos).length();
    const double dn = (max_dist > 1e-9) ? std::clamp(d / max_dist, 0.0, 1.0) : 0.0;

    int hop = 0;
    if (auto it = hops.find(sid); it != hops.end()) hop = std::max(0, it->second);
    const double hn = std::clamp(static_cast<double>(hop) / 6.0, 0.0, 1.0);

    // Base desirability: prefer "interesting" regions and a little distance.
    double w = 0.25 + 1.10 * ruins + 0.20 * std::clamp(salvage_mult - 1.0, -1.0, 1.0) +
               0.25 * dn + 0.15 * hn;

    if (kind == LeadKind::HiddenCache) {
      // Caches skew toward pirate/salvage-rich regions.
      w *= (0.70 + 0.90 * pirate) * (0.75 + 0.50 * salvage_mult);
    } else if (kind == LeadKind::StarChart) {
      // Charts skew a bit farther out.
      w *= (0.75 + 0.65 * dn + 0.35 * hn) * (0.85 + 0.20 * salvage_mult);
    } else {
      // Follow-up anomalies skew toward ruins.
      w *= (0.80 + 0.90 * ruins) * (0.90 + 0.15 * salvage_mult);
    }

    if (!std::isfinite(w) || w < 0.0) w = 0.0;
    weights.push_back(w);
    total_w += w;
  }

  if (!(total_w > 1e-12)) {
    // Fall back to deterministic selection.
    return !candidates.empty() ? candidates.front() : kInvalidId;
  }

  double r = rng.next_u01() * total_w;
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    r -= weights[i];
    if (r <= 0.0) return candidates[i];
  }
  return candidates.back();
}

static LeadOutcome maybe_spawn_anomaly_lead(Simulation& sim, const Ship& resolver, const Anomaly& resolved) {
  LeadOutcome out;
  const auto& cfg = sim.cfg();
  if (!cfg.enable_anomaly_leads) return out;

  if (resolved.resolved_by_faction_id == kInvalidId) return out;

  // Cap chain depth.
  if (cfg.anomaly_lead_max_depth >= 0 && resolved.lead_depth >= cfg.anomaly_lead_max_depth) return out;

  GameState& s = sim.state();
  auto* fac = find_ptr(s.factions, resolver.faction_id);
  if (!fac) return out;

  // Don't generate if the galaxy has nowhere to point.
  if (s.systems.size() < 2) return out;

  // Global cap on generated anomalies.
  if (cfg.anomaly_lead_max_total_generated > 0) {
    int generated = 0;
    for (const auto& [_, a] : s.anomalies) {
      if (a.lead_depth > 0) ++generated;
    }
    if (generated >= cfg.anomaly_lead_max_total_generated) return out;
  }

  // Deterministic seed based on the resolved anomaly + resolver identity.
  std::uint64_t seed = 0x6d0f27bd9c2b3f61ULL;
  seed ^= static_cast<std::uint64_t>(resolved.id) * 0x9e3779b97f4a7c15ULL;
  seed ^= static_cast<std::uint64_t>(resolver.id) * 0xbf58476d1ce4e5b9ULL;
  seed ^= static_cast<std::uint64_t>(resolver.faction_id) * 0x94d049bb133111ebULL;
  seed ^= static_cast<std::uint64_t>(resolved.resolved_day) * 0x2545f4914f6cdd1dULL;
  HashRng rng(splitmix64(seed));

  // Trigger probability: base + small bonuses for "richer" anomalies.
  double p = std::clamp(cfg.anomaly_lead_base_chance, 0.0, 1.0);
  if (resolved.research_reward > 1e-9) p += 0.03;
  if (!resolved.unlock_component_id.empty()) p += 0.05;
  if (!resolved.mineral_reward.empty()) p += 0.03;
  if (resolved.hazard_chance > 1e-9) p += 0.02;
  p = std::clamp(p, 0.0, 0.95);

  if (rng.next_u01() >= p) return out;

  // Determine lead type.
  const double p_star = std::clamp(cfg.anomaly_lead_star_chart_chance, 0.0, 1.0);
  const double p_cache = std::clamp(cfg.anomaly_lead_hidden_cache_chance, 0.0, 1.0);
  double r = rng.next_u01();

  LeadKind kind = LeadKind::FollowUpAnomaly;
  if (r < p_star) kind = LeadKind::StarChart;
  else if (r < p_star + p_cache) kind = LeadKind::HiddenCache;

  // Precompute hop distances and max galaxy distance for weighting.
  const auto hop_map = compute_system_hops(s, resolver.system_id);
  const auto* origin_sys = find_ptr(s.systems, resolver.system_id);
  if (!origin_sys) return out;

  double max_dist = 0.0;
  for (Id sid : sorted_keys(s.systems)) {
    if (sid == resolver.system_id) continue;
    if (const auto* sys = find_ptr(s.systems, sid)) {
      max_dist = std::max(max_dist, (sys->galaxy_pos - origin_sys->galaxy_pos).length());
    }
  }

  const bool use_hop_filter = (cfg.anomaly_lead_min_hops > 0 || cfg.anomaly_lead_max_hops > 0);
  const int min_h = std::max(0, cfg.anomaly_lead_min_hops);
  const int max_h = (cfg.anomaly_lead_max_hops > 0) ? std::max(min_h, cfg.anomaly_lead_max_hops) : 9999;

  auto build_candidates = [&](bool prefer_undiscovered, bool hop_filter) -> std::vector<Id> {
    std::vector<Id> cand;
    cand.reserve(s.systems.size());
    for (Id sid : sorted_keys(s.systems)) {
      if (sid == resolver.system_id) continue;
      if (!find_ptr(s.systems, sid)) continue;

      const bool discovered = (std::find(fac->discovered_systems.begin(), fac->discovered_systems.end(), sid) !=
                               fac->discovered_systems.end());

      if (prefer_undiscovered && discovered) continue;
      if (!prefer_undiscovered && !discovered) continue;

      if (hop_filter) {
        auto it = hop_map.find(sid);
        if (it == hop_map.end()) continue;
        const int h = std::max(0, it->second);
        if (h < min_h || h > max_h) continue;
      }

      cand.push_back(sid);
    }
    return cand;
  };

  // Candidate selection strategy:
  //   StarChart: prefer undiscovered systems in hop window, else relax.
  //   FollowUp/Cache: prefer discovered systems in hop window, else relax.
  std::vector<Id> candidates;
  if (kind == LeadKind::StarChart) {
    candidates = build_candidates(/*prefer_undiscovered=*/true, /*hop_filter=*/use_hop_filter);
    if (candidates.empty()) candidates = build_candidates(/*prefer_undiscovered=*/true, /*hop_filter=*/false);
    if (candidates.empty()) candidates = build_candidates(/*prefer_undiscovered=*/false, /*hop_filter=*/use_hop_filter);
    if (candidates.empty()) candidates = build_candidates(/*prefer_undiscovered=*/false, /*hop_filter=*/false);
  } else {
    candidates = build_candidates(/*prefer_undiscovered=*/false, /*hop_filter=*/use_hop_filter);
    if (candidates.empty()) candidates = build_candidates(/*prefer_undiscovered=*/false, /*hop_filter=*/false);
    if (candidates.empty()) candidates = build_candidates(/*prefer_undiscovered=*/true, /*hop_filter=*/use_hop_filter);
    if (candidates.empty()) candidates = build_candidates(/*prefer_undiscovered=*/true, /*hop_filter=*/false);
  }

  if (candidates.empty()) return out;

  // Try a few times in case we pick an unreachable target in a disconnected galaxy.
  for (int attempt = 0; attempt < 5 && !candidates.empty(); ++attempt) {
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

    const Id target = pick_weighted_system(rng, candidates, s, resolver.system_id, hop_map, max_dist, kind);
    if (target == kInvalidId) return out;

    const auto plan = sim.plan_jump_route_from_pos(resolver.system_id, resolver.position_mkm, resolver.faction_id,
                                                   std::max(1e-9, resolver.speed_km_s), target,
                                                   /*restrict_to_discovered=*/false);
    if (!plan.has_value()) {
      // Remove and retry.
      candidates.erase(std::remove(candidates.begin(), candidates.end(), target), candidates.end());
      continue;
    }

    out.kind = kind;
    out.target_system_id = target;
    out.hops = std::max(0, static_cast<int>(plan->jump_ids.size()));

    const bool target_discovered =
        (std::find(fac->discovered_systems.begin(), fac->discovered_systems.end(), target) != fac->discovered_systems.end());
    out.revealed_new_system = !target_discovered;

    // Reveal route intel if needed: charts always, other leads only when the target is undiscovered.
    const bool reveal = (kind == LeadKind::StarChart) || !target_discovered;
    if (reveal) {
      sim.reveal_route_intel_for_faction(resolver.faction_id, plan->systems, plan->jump_ids);
      out.revealed_route = true;
    }

    // Spawn the follow-up site.
    if (kind == LeadKind::HiddenCache) {
      if (!cfg.enable_wrecks) return out; // can't realize this lead type.

      Wreck w;
      w.id = allocate_id(s);
      w.system_id = target;
      w.position_mkm = pick_site_position_mkm(s, target, rng);
      w.kind = WreckKind::Cache;
      w.created_day = static_cast<int>(s.date.days_since_epoch());
      w.name = procgen_obscure::generate_wreck_cache_name(w, "Hidden");

      // Cache size scales a bit with hop distance.
      const double scale = 1.0 + 0.20 * std::min(6, out.hops);
      w.minerals = generate_mineral_bundle(rng, /*scale=*/2.2 * scale);

      // Make sure it's worth a trip.
      double total = 0.0;
      for (const auto& [_, t] : w.minerals) total += std::max(0.0, t);
      if (!(total > 1e-3)) w.minerals["Duranium"] = 80.0 * scale;

      const Id new_wreck_id = w.id;
      s.wrecks.emplace(new_wreck_id, std::move(w));
      out.spawned_wreck_id = new_wreck_id;
      return out;
    }

    // Follow-up anomaly site.
    {
      Anomaly a;
      a.id = allocate_id(s);
      a.system_id = target;
      a.position_mkm = pick_site_position_mkm(s, target, rng);

      const int depth = std::max(0, resolved.lead_depth + 1);
      a.origin_anomaly_id = resolved.id;
      a.lead_depth = depth;

      // Kind/name are lightweight narrative tags; keep short for UI.
      const double t = rng.next_u01();
      if (kind == LeadKind::StarChart) {
        a.kind = (t < 0.55) ? "ruins" : ((t < 0.80) ? "artifact" : "signal");
      } else {
        a.kind = (t < 0.45) ? "signal" : ((t < 0.75) ? "ruins" : "phenomenon");
      }

      // Obscure procedural naming. Lead-chains remain coherent via origin_anomaly_id.
      a.name = procgen_obscure::generate_anomaly_name(a);

      // Investigation time and rewards scale gently by hops/depth.
      a.investigation_days = std::max(1, 3 + rng.range_int(0, 6) + depth);
      const double hop_scale = 1.0 + 0.12 * std::min(6, out.hops);
      const double depth_scale = 1.0 + 0.10 * std::max(0, depth - 1);
      a.research_reward = std::max(0.0, rng.range(10.0, 55.0) * hop_scale * depth_scale);

      // Optional mineral reward.
      if (rng.next_u01() < 0.55) {
        a.mineral_reward = generate_mineral_bundle(rng, /*scale=*/1.3 * hop_scale);
      }

      // Optional component unlock (rarer for deeper chains).
      if (rng.next_u01() < (0.28 / std::max(1, depth))) {
        a.unlock_component_id = pick_unlock_component_id(sim.content(), *fac, rng);
      }

      // Small hazard risk (non-lethal).
      if (rng.next_u01() < 0.55) {
        a.hazard_chance = rng.range(0.10, 0.35);
        a.hazard_damage = rng.range(0.5, 4.5) * hop_scale;
      }

      s.anomalies[a.id] = a;
      // Mark as known to the resolving faction (intel from the original anomaly).
      push_unique(fac->discovered_anomalies, a.id);
      out.spawned_anomaly_id = a.id;
    }

    return out;
  }

  return out;
}


struct CodexEchoOutcome {
  Id root_anomaly_id{kInvalidId};
  int fragments_have{0};
  int fragments_required{0};

  Id target_system_id{kInvalidId};
  int hops{0};
  bool revealed_new_system{false};
  bool revealed_route{false};

  Id spawned_anomaly_id{kInvalidId};
  Id offered_contract_id{kInvalidId};
};

static bool has_codex_echo_for_root(const GameState& s, Id root_id) {
  if (root_id == kInvalidId) return false;
  for (const auto& [_, a] : s.anomalies) {
    if (a.kind == "codex_echo" && a.origin_anomaly_id == root_id) return true;
  }
  return false;
}

static std::optional<CodexEchoOutcome> maybe_trigger_codex_echo(Simulation& sim,
                                                                const Ship& resolver,
                                                                const Anomaly& resolved) {
  const auto& cfg = sim.cfg();
  if (!cfg.enable_obscure_codex_fragments || !cfg.enable_codex_echo_reward) return std::nullopt;
  if (resolver.faction_id == kInvalidId || resolved.resolved_by_faction_id == kInvalidId) return std::nullopt;

  GameState& s = sim.state();
  auto* fac = find_ptr(s.factions, resolver.faction_id);
  if (!fac) return std::nullopt;

  const Id root = procgen_obscure::anomaly_chain_root_id(s.anomalies, resolved.id);
  const int req = std::max(1, cfg.codex_fragments_required);
  const int have = procgen_obscure::faction_resolved_anomaly_chain_count(s.anomalies, resolver.faction_id, root);
  if (have < req) return std::nullopt;

  if (has_codex_echo_for_root(s, root)) return std::nullopt;

  const auto* origin_sys = find_ptr(s.systems, resolver.system_id);
  if (!origin_sys) return std::nullopt;

  // Determine reachable candidate systems.
  const auto hop_map = compute_system_hops(s, resolver.system_id);

  double max_dist = 0.0;
  for (Id sid : sorted_keys(s.systems)) {
    if (sid == resolver.system_id) continue;
    if (const auto* sys = find_ptr(s.systems, sid)) {
      max_dist = std::max(max_dist, (sys->galaxy_pos - origin_sys->galaxy_pos).length());
    }
  }

  const int min_h = std::max(0, cfg.codex_echo_min_hops);
  const int max_h = (cfg.codex_echo_max_hops > 0) ? std::max(min_h, cfg.codex_echo_max_hops) : 9999;
  const bool use_hop_filter = (cfg.codex_echo_min_hops > 0 || cfg.codex_echo_max_hops > 0);

  auto build_candidates = [&](bool prefer_undiscovered, bool hop_filter) -> std::vector<Id> {
    std::vector<Id> cand;
    cand.reserve(s.systems.size());
    for (Id sid : sorted_keys(s.systems)) {
      if (sid == resolver.system_id) continue;
      if (!find_ptr(s.systems, sid)) continue;

      const bool discovered = std::find(fac->discovered_systems.begin(), fac->discovered_systems.end(), sid) !=
                              fac->discovered_systems.end();
      if (prefer_undiscovered && discovered) continue;
      if (!prefer_undiscovered && !discovered) continue;

      if (hop_filter) {
        auto it = hop_map.find(sid);
        if (it == hop_map.end()) continue;
        const int h = std::max(0, it->second);
        if (h < min_h || h > max_h) continue;
      }

      cand.push_back(sid);
    }
    return cand;
  };

  std::vector<Id> candidates = build_candidates(/*prefer_undiscovered=*/true, /*hop_filter=*/use_hop_filter);
  if (candidates.empty()) candidates = build_candidates(/*prefer_undiscovered=*/true, /*hop_filter=*/false);
  if (candidates.empty()) candidates = build_candidates(/*prefer_undiscovered=*/false, /*hop_filter=*/use_hop_filter);
  if (candidates.empty()) candidates = build_candidates(/*prefer_undiscovered=*/false, /*hop_filter=*/false);
  if (candidates.empty()) return std::nullopt;

  // Deterministic RNG seed keyed on chain root + faction.
  std::uint64_t seed = 0xC0DEC0DEC0DEC0DEULL;
  seed ^= static_cast<std::uint64_t>(root) * 0x9e3779b97f4a7c15ULL;
  seed ^= static_cast<std::uint64_t>(resolver.faction_id) * 0xbf58476d1ce4e5b9ULL;
  seed ^= static_cast<std::uint64_t>(s.date.days_since_epoch()) * 0x94d049bb133111ebULL;
  HashRng rng(splitmix64(seed));

  CodexEchoOutcome out;
  out.root_anomaly_id = root;
  out.fragments_have = have;
  out.fragments_required = req;

  for (int attempt = 0; attempt < 5 && !candidates.empty(); ++attempt) {
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

    const Id target = pick_weighted_system(rng, candidates, s, resolver.system_id, hop_map, max_dist, LeadKind::StarChart);
    if (target == kInvalidId) break;

    const auto plan = sim.plan_jump_route_from_pos(resolver.system_id, resolver.position_mkm, resolver.faction_id,
                                                   std::max(1e-9, resolver.speed_km_s), target,
                                                   /*restrict_to_discovered=*/false);
    if (!plan.has_value()) {
      candidates.erase(std::remove(candidates.begin(), candidates.end(), target), candidates.end());
      continue;
    }

    out.target_system_id = target;
    out.hops = std::max(0, static_cast<int>(plan->jump_ids.size()));

    const bool target_discovered =
        std::find(fac->discovered_systems.begin(), fac->discovered_systems.end(), target) != fac->discovered_systems.end();
    out.revealed_new_system = !target_discovered;

    // Codex echo always reveals route intel (like an explicit chart).
    sim.reveal_route_intel_for_faction(resolver.faction_id, plan->systems, plan->jump_ids);
    out.revealed_route = true;

    // Spawn the echo site.
    Anomaly a;
    a.id = allocate_id(s);
    a.system_id = target;
    a.position_mkm = pick_site_position_mkm(s, target, rng);
    a.origin_anomaly_id = root;
    a.lead_depth = std::max(0, resolved.lead_depth + 1);

    a.kind = "codex_echo";
    a.name = procgen_obscure::anomaly_theme_label(a) + ": Codex Echo";

    // Make it feel special but not wildly out of band.
    a.investigation_days = std::max(1, 3 + rng.range_int(0, 6));
    const double hop_scale = 1.0 + 0.12 * std::min(6, out.hops);
    a.research_reward = std::max(0.0, rng.range(25.0, 85.0) * hop_scale);

    if (rng.next_u01() < 0.65) {
      a.mineral_reward = generate_mineral_bundle(rng, /*scale=*/1.8 * hop_scale);
    }
    if (rng.next_u01() < 0.35) {
      a.unlock_component_id = pick_unlock_component_id(sim.content(), *fac, rng);
    }
    if (rng.next_u01() < 0.60) {
      a.hazard_chance = rng.range(0.12, 0.35);
      a.hazard_damage = rng.range(0.8, 5.0) * hop_scale;
    }

    s.anomalies[a.id] = a;
    push_unique(fac->discovered_anomalies, a.id);
    out.spawned_anomaly_id = a.id;

    // Optional contract offer.
    if (cfg.enable_contracts && cfg.codex_echo_offer_contract) {
      Contract c;
      c.id = allocate_id(s);
      c.kind = ContractKind::InvestigateAnomaly;
      c.status = ContractStatus::Offered;
      c.issuer_faction_id = resolver.faction_id;
      c.assignee_faction_id = resolver.faction_id;
      c.system_id = target;
      c.target_id = a.id;
      c.offered_day = s.date.days_since_epoch();
      if (cfg.contract_offer_expiry_days > 0) {
        c.expires_day = c.offered_day + cfg.contract_offer_expiry_days;
      }
      c.hops_estimate = out.hops;
      // Risk estimate: crude proxy from hazard chance.
      c.risk_estimate = std::clamp(a.hazard_chance, 0.0, 1.0);

      c.name = "Codex Echo: Investigate " + a.name;

      // Reward: based on anomaly value + a bonus for being a codex completion.
      const double base = std::max(0.0, cfg.contract_reward_base_rp) +
                          std::max(0.0, cfg.codex_echo_contract_bonus_rp) +
                          static_cast<double>(std::max(0, c.hops_estimate)) * std::max(0.0, cfg.contract_reward_rp_per_hop) +
                          c.risk_estimate * std::max(0.0, cfg.contract_reward_rp_per_risk);
      c.reward_research_points = base + 0.15 * std::max(0.0, a.research_reward);

      s.contracts.emplace(c.id, c);
      out.offered_contract_id = c.id;
    }

    return out;
  }

  return std::nullopt;
}

} // namespace

void Simulation::tick_ships(double dt_days) {
  dt_days = std::clamp(dt_days, 0.0, 10.0);
  NEBULA4X_TRACE_SCOPE("tick_ships", "sim.ships");

  // Missile ships can carry Munitions for reloading without it consuming their
  // generic cargo capacity. Treat Munitions as magazine stock for finite-ammo
  // missile designs.
  constexpr const char* kMunitionsKey = "Munitions";

  auto cargo_used_tons = [&](const Ship& s) {
    double used = 0.0;
    bool ignore_munitions = false;
    if (const ShipDesign* d = find_design(s.design_id)) {
      ignore_munitions = d->missile_ammo_capacity > 0;
    }
    for (const auto& [k, tons] : s.cargo) {
      if (ignore_munitions && k == kMunitionsKey) continue;
      used += std::max(0.0, tons);
    }
    return used;
  };

  auto munitions_magazine_free_tons = [&](const Ship& s) -> double {
    const ShipDesign* d = find_design(s.design_id);
    if (!d) return 0.0;
    const int cap = std::max(0, d->missile_ammo_capacity);
    if (cap <= 0) return 0.0;
    int ammo = s.missile_ammo;
    if (ammo < 0) ammo = cap;
    ammo = std::clamp(ammo, 0, cap);

    double stored = 0.0;
    if (auto it = s.cargo.find(kMunitionsKey); it != s.cargo.end()) {
      stored = std::max(0.0, it->second);
    }

    double free = static_cast<double>(cap) - static_cast<double>(ammo) - stored;
    if (!std::isfinite(free)) return 0.0;
    return std::max(0.0, free);
  };

  auto reload_missile_ammo_from_munitions = [&](Ship& s) {
    const ShipDesign* d = find_design(s.design_id);
    if (!d) return;
    const int cap = std::max(0, d->missile_ammo_capacity);
    if (cap <= 0) return;

    if (s.missile_ammo < 0) s.missile_ammo = cap;
    s.missile_ammo = std::clamp(s.missile_ammo, 0, cap);
    int need = cap - s.missile_ammo;
    if (need <= 0) return;

    auto it = s.cargo.find(kMunitionsKey);
    if (it == s.cargo.end()) return;
    const double avail_d = std::max(0.0, it->second);
    const int avail = static_cast<int>(std::floor(avail_d + 1e-9));
    const int take = std::min(need, avail);
    if (take <= 0) return;

    s.missile_ammo += take;
    s.missile_ammo = std::clamp(s.missile_ammo, 0, cap);

    it->second = avail_d - static_cast<double>(take);
    if (it->second <= 1e-9) s.cargo.erase(it);
  };

  const double arrive_eps = std::max(0.0, cfg_.arrival_epsilon_mkm);
  const double dock_range = std::max(arrive_eps, cfg_.docking_range_mkm);

  // Merchant Guild (civilian trade convoys) faction id cache.
  constexpr const char* kMerchantFactionName = "Merchant Guild";
  Id merchant_faction_id = kInvalidId;
  for (const auto& [fid, f] : state_.factions) {
    if (f.control == FactionControl::AI_Passive && f.name == kMerchantFactionName) {
      merchant_faction_id = fid;
      break;
    }
  }

  const bool allow_civilian_trade_cargo_ops =
      cfg_.enable_civilian_trade_convoys && cfg_.enable_civilian_trade_convoy_cargo_transfers &&
      merchant_faction_id != kInvalidId;

  // Cache: faction -> (colony -> mineral -> desired reserve tons) derived from
  // logistics_needs_for_faction(). Used to keep civilian exports from starving
  // shipyards / industry / rearm buffers even when the colony has no explicit
  // mineral_reserves/mineral_targets set.
  struct LogisticsReserveCache {
    bool built{false};
    std::unordered_map<Id, std::unordered_map<std::string, double>> reserve_by_colony;
  };

  std::unordered_map<Id, LogisticsReserveCache> logistics_reserve_cache;
  logistics_reserve_cache.reserve(state_.factions.size() * 2 + 8);

  auto logistics_reserve_tons = [&](Id faction_id, Id colony_id, const std::string& mineral) -> double {
    if (faction_id == kInvalidId || colony_id == kInvalidId) return 0.0;
    auto& entry = logistics_reserve_cache[faction_id];
    if (!entry.built) {
      entry.built = true;
      const auto needs = logistics_needs_for_faction(faction_id);
      for (const auto& n : needs) {
        if (n.colony_id == kInvalidId) continue;
        const double desired = std::max(0.0, n.desired_tons);
        if (desired <= 1e-9) continue;
        double& r = entry.reserve_by_colony[n.colony_id][n.mineral];
        r = std::max(r, desired);
      }
    }

    auto itc = entry.reserve_by_colony.find(colony_id);
    if (itc == entry.reserve_by_colony.end()) return 0.0;
    auto itm = itc->second.find(mineral);
    if (itm == itc->second.end()) return 0.0;
    return std::max(0.0, itm->second);
  };

  const double maint_min_speed = std::clamp(cfg_.ship_maintenance_min_speed_multiplier, 0.0, 1.0);
  auto maintenance_speed_mult = [&](const Ship& s) -> double {
    if (!cfg_.enable_ship_maintenance) return 1.0;
    double m = s.maintenance_condition;
    if (!std::isfinite(m)) m = 1.0;
    m = std::clamp(m, 0.0, 1.0);
    return maint_min_speed + (1.0 - maint_min_speed) * m;
  };

  const auto ship_ids = sorted_keys(state_.ships);

  // Capture pre-move positions so we can compute per-ship velocities after all
  // movement/order processing completes.
  //
  // We do this as a single prepass so we don't have to carefully update velocity
  // in every early-continue branch of the (large) ship order state machine.
  std::unordered_map<Id, Vec2> pre_pos_mkm;
  std::unordered_map<Id, Id> pre_sys;
  pre_pos_mkm.reserve(ship_ids.size() * 2);
  pre_sys.reserve(ship_ids.size() * 2);
  for (Id sid : ship_ids) {
    const auto* sh = find_ptr(state_.ships, sid);
    if (!sh) continue;
    pre_pos_mkm.emplace(sid, sh->position_mkm);
    pre_sys.emplace(sid, sh->system_id);
  }

  // --- Invasion orbital control cache ---
  //
  // Troop landings at hostile colonies are throughput-limited. When blockades
  // are enabled, we additionally scale *invasion* landing throughput by a
  // lightweight "orbital control" fraction derived from nearby combat power.
  //
  // This is computed using the *pre-move* positions captured above to keep the
  // result deterministic within a tick (independent of ship processing order).

  struct InvasionOrbitalKey {
    Id colony_id{kInvalidId};
    Id attacker_faction_id{kInvalidId};

    bool operator==(const InvasionOrbitalKey& o) const {
      return colony_id == o.colony_id && attacker_faction_id == o.attacker_faction_id;
    }
  };

  struct InvasionOrbitalKeyHash {
    size_t operator()(const InvasionOrbitalKey& k) const {
      std::uint64_t h = 1469598103934665603ull;
      auto mix = [&](std::uint64_t v) {
        h ^= v;
        h *= 1099511628211ull;
      };
      mix(k.colony_id);
      mix(k.attacker_faction_id);
      return static_cast<size_t>(h);
    }
  };

  std::unordered_map<InvasionOrbitalKey, double, InvasionOrbitalKeyHash> invasion_orbital_control_cache;
  invasion_orbital_control_cache.reserve(32);

  auto invasion_orbital_control = [&](Id colony_id, Id attacker_faction_id) -> double {
    if (!cfg_.enable_blockades) return 1.0;
    if (colony_id == kInvalidId || attacker_faction_id == kInvalidId) return 0.0;

    const InvasionOrbitalKey key{colony_id, attacker_faction_id};
    if (auto it = invasion_orbital_control_cache.find(key); it != invasion_orbital_control_cache.end()) {
      return it->second;
    }

    const Colony* col = find_ptr(state_.colonies, colony_id);
    const Body* body = col ? find_ptr(state_.bodies, col->body_id) : nullptr;
    const Id sys_id = body ? body->system_id : kInvalidId;
    if (!col || !body || sys_id == kInvalidId) {
      invasion_orbital_control_cache.emplace(key, 0.0);
      return 0.0;
    }

    const Vec2 anchor = body->position_mkm;
    const double radius_mkm = std::max(0.0, cfg_.blockade_radius_mkm);
    const double base_resist = std::max(0.0, cfg_.blockade_base_resistance_power);

    auto ship_power = [&](const Ship& sh) -> double {
      if (sh.hp <= 1e-9) return 0.0;
      const ShipDesign* d = find_design(sh.design_id);
      if (!d) return 0.0;
      const double w = std::max(0.0, d->weapon_damage) + std::max(0.0, d->missile_damage) +
                       0.5 * std::max(0.0, d->point_defense_damage);
      if (w <= 1e-9) return 0.0;
      const double dur = 0.05 * (std::max(0.0, sh.hp) + std::max(0.0, sh.shields));
      const double sen = 0.25 * std::max(0.0, d->sensor_range_mkm);
      return w + dur + sen;
    };

    double attacker_power = 0.0;
    double defender_power = 0.0;

    // Ships near the target at the start of the tick.
    for (Id sid : ship_ids) {
      const Ship* sh = find_ptr(state_.ships, sid);
      if (!sh) continue;
      if (sh->faction_id == kInvalidId) continue;

      auto its = pre_sys.find(sid);
      if (its == pre_sys.end() || its->second != sys_id) continue;

      if (radius_mkm > 1e-9) {
        auto itp = pre_pos_mkm.find(sid);
        if (itp == pre_pos_mkm.end()) continue;
        if ((itp->second - anchor).length() > radius_mkm + 1e-9) continue;
      }

      const double p = ship_power(*sh);
      if (p <= 1e-9) continue;

      if (sh->faction_id == attacker_faction_id) {
        attacker_power += p;
      } else if (sh->faction_id == col->faction_id || are_factions_trade_partners(col->faction_id, sh->faction_id)) {
        defender_power += p;
      } else {
        // Third parties are ignored for this simple orbital-control estimator.
      }
    }

    // Static weapons contribute to defender orbital resistance.
    double static_power = 0.0;
    for (const auto& [inst_id, count] : col->installations) {
      if (count <= 0) continue;
      const auto it = content_.installations.find(inst_id);
      if (it == content_.installations.end()) continue;
      const double wd = std::max(0.0, it->second.weapon_damage);
      const double pd = std::max(0.0, it->second.point_defense_damage);
      const double w = wd + 0.5 * pd;
      if (w <= 1e-9) continue;
      static_power += w * static_cast<double>(count);
    }
    defender_power += static_power;

    attacker_power = std::max(0.0, attacker_power);
    defender_power = std::max(0.0, defender_power);

    const double denom = attacker_power + defender_power + base_resist;
    double control = (denom > 1e-9) ? (attacker_power / denom) : 0.0;
    if (!std::isfinite(control)) control = 0.0;
    control = std::clamp(control, 0.0, 1.0);

    invasion_orbital_control_cache.emplace(key, control);
    return control;
  };

  auto can_refill_from_repeat = [](const ShipOrders& so) {
    return so.repeat && !so.repeat_template.empty() && so.repeat_count_remaining != 0;
  };

  auto is_player_faction = [&](Id faction_id) -> bool {
    const Faction* f = find_ptr(state_.factions, faction_id);
    return f && f->control == FactionControl::Player;
  };

  auto ship_hp_fraction = [&](const Ship& sh) -> double {
    const ShipDesign* d = find_design(sh.design_id);
    const double max_hp = (d && d->max_hp > 1e-9) ? d->max_hp : sh.hp;
    if (max_hp <= 1e-9) return 1.0;
    double f = sh.hp / max_hp;
    if (!std::isfinite(f)) f = 1.0;
    return std::clamp(f, 0.0, 1.0);
  };

  auto missile_ammo_fraction = [&](const Ship& sh) -> double {
    const ShipDesign* d = find_design(sh.design_id);
    const double cap = (d && d->missile_ammo_capacity > 0.0) ? d->missile_ammo_capacity : 0.0;
    if (cap <= 1e-9) return 1.0;
    double a = std::max(0.0, static_cast<double>(sh.missile_ammo));
    double f = a / cap;
    if (!std::isfinite(f)) f = 1.0;
    return std::clamp(f, 0.0, 1.0);
  };

  auto any_friendly_colony_in_system = [&](Id ship_faction_id, Id system_id) -> bool {
    if (ship_faction_id == kInvalidId || system_id == kInvalidId) return false;
    for (const auto& [cid, col] : state_.colonies) {
      if (!are_factions_trade_partners(ship_faction_id, col.faction_id)) continue;
      const Body* b = find_ptr(state_.bodies, col.body_id);
      if (!b || b->system_id != system_id) continue;
      return true;
    }
    return false;
  };

  auto build_emergency_retreat_plan = [&](const Ship& sh,
                                         const std::vector<Id>& detected_hostiles) -> std::vector<Order> {
    std::vector<Order> out;

    // If we have a friendly colony with a shipyard within a known route, prefer it.
    struct ColonyCandidate {
      Id colony_id{kInvalidId};
      Id body_id{kInvalidId};
      Id system_id{kInvalidId};
      bool has_shipyard{false};
      bool system_hostile{false};
      JumpRoutePlan plan;
    };

    std::vector<ColonyCandidate> candidates;
    candidates.reserve(state_.colonies.size());

    // Use a speed estimate that accounts for damage-related multipliers.
    double plan_speed_km_s = sh.speed_km_s;
    plan_speed_km_s *= maintenance_speed_mult(sh);
    plan_speed_km_s *= ship_heat_speed_multiplier(sh);
    plan_speed_km_s *= ship_subsystem_engine_multiplier(sh);
    if (!std::isfinite(plan_speed_km_s) || plan_speed_km_s < 0.0) plan_speed_km_s = 0.0;

    for (const auto& [cid, col] : state_.colonies) {
      if (sh.faction_id == kInvalidId) continue;
      if (!are_factions_trade_partners(sh.faction_id, col.faction_id)) continue;
      const Body* b = find_ptr(state_.bodies, col.body_id);
      if (!b || b->system_id == kInvalidId) continue;

      // Plan a route to the current position of the colony body.
      const auto plan_opt = plan_jump_route_cached(sh.system_id, sh.position_mkm, sh.faction_id, plan_speed_km_s,
                                                   b->system_id, /*restrict_to_discovered=*/true, b->position_mkm);
      if (!plan_opt) continue;

      ColonyCandidate cand;
      cand.colony_id = cid;
      cand.body_id = col.body_id;
      cand.system_id = b->system_id;
      cand.plan = *plan_opt;
      cand.has_shipyard = false;
      if (auto it = col.installations.find("shipyard"); it != col.installations.end()) {
        cand.has_shipyard = it->second > 0;
      }
      cand.system_hostile =
          (sh.faction_id != kInvalidId) && (!detected_hostile_ships_in_system(sh.faction_id, cand.system_id).empty());
      candidates.push_back(std::move(cand));
    }

    auto pick_best = [&](auto pred) -> std::optional<ColonyCandidate> {
      std::optional<ColonyCandidate> best;
      for (const auto& c : candidates) {
        if (!pred(c)) continue;
        if (!best || c.plan.total_eta_days < best->plan.total_eta_days) {
          best = c;
        }
      }
      return best;
    };

    std::optional<ColonyCandidate> chosen;
    chosen = pick_best([](const ColonyCandidate& c) { return c.has_shipyard && !c.system_hostile; });
    if (!chosen) chosen = pick_best([](const ColonyCandidate& c) { return !c.system_hostile; });
    if (!chosen) chosen = pick_best([](const ColonyCandidate& c) { return c.has_shipyard; });
    if (!chosen) chosen = pick_best([](const ColonyCandidate&) { return true; });

    if (chosen && chosen->body_id != kInvalidId) {
      // Jump legs first...
      for (Id jid : chosen->plan.jump_ids) {
        if (jid != kInvalidId) out.push_back(TravelViaJump{jid});
      }
      // ...then dock at the colony body.
      out.push_back(MoveToBody{chosen->body_id});
      return out;
    }

    // Otherwise, flee via the best known jump point.
    const StarSystem* sys = find_ptr(state_.systems, sh.system_id);
    if (sys) {
      struct JumpCandidate {
        Id jump_id{kInvalidId};
        bool dest_hostile{false};
        bool dest_has_friendly_colony{false};
        double dist{0.0};
      };

      std::vector<JumpCandidate> jc;
      jc.reserve(sys->jump_points.size());

      for (Id jid : sys->jump_points) {
        const JumpPoint* jp = find_ptr(state_.jump_points, jid);
        if (!jp) continue;
        const JumpPoint* linked = (jp->linked_jump_id != kInvalidId) ? find_ptr(state_.jump_points, jp->linked_jump_id)
                                                                     : nullptr;
        const Id dest_sys = linked ? linked->system_id : kInvalidId;

        JumpCandidate c;
        c.jump_id = jid;
        c.dist = (sh.position_mkm - jp->position_mkm).length();
        if (!std::isfinite(c.dist)) c.dist = 1e18;

        c.dest_hostile = (dest_sys != kInvalidId && sh.faction_id != kInvalidId) &&
                         (!detected_hostile_ships_in_system(sh.faction_id, dest_sys).empty());
        c.dest_has_friendly_colony = (dest_sys != kInvalidId) && any_friendly_colony_in_system(sh.faction_id, dest_sys);
        jc.push_back(c);
      }

      auto better = [&](const JumpCandidate& a, const JumpCandidate& b) {
        // Prefer non-hostile destinations, then destinations with friendly colonies, then shorter distance.
        const auto key = [](const JumpCandidate& c) {
          return std::tuple<int, int, double>(c.dest_hostile ? 1 : 0, c.dest_has_friendly_colony ? 0 : 1, c.dist);
        };
        return key(a) < key(b);
      };

      std::optional<JumpCandidate> best;
      for (const auto& c : jc) {
        if (!best || better(c, *best)) best = c;
      }

      if (best && best->jump_id != kInvalidId) {
        out.push_back(TravelViaJump{best->jump_id});
        return out;
      }
    }

    // Last resort: move directly away from the centroid of detected hostiles.
    Vec2 centroid;
    int n = 0;
    for (Id hid : detected_hostiles) {
      const Ship* hs = find_ptr(state_.ships, hid);
      if (!hs || hs->system_id != sh.system_id) continue;
      centroid += hs->position_mkm;
      ++n;
    }
    if (n > 0) centroid = centroid * (1.0 / static_cast<double>(n));

    Vec2 dir = sh.position_mkm - centroid;
    if (dir.length() <= 1e-9) dir = Vec2{1.0, 0.0};
    dir = dir.normalized();
    const double flee_dist = 200.0;  // mkm
    out.push_back(MoveToPoint{sh.position_mkm + dir * flee_dist});
    return out;
  };

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
    Escort,
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
    if (std::holds_alternative<EscortShip>(ord)) {
      k.kind = CohortKind::Escort;
      k.target_id = std::get<EscortShip>(ord).target_ship_id;
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
    if (std::holds_alternative<LoadTroops>(ord)) {
      k.kind = CohortKind::Load;
      k.target_id = std::get<LoadTroops>(ord).colony_id;
      return k;
    }
    if (std::holds_alternative<UnloadTroops>(ord)) {
      k.kind = CohortKind::Unload;
      k.target_id = std::get<UnloadTroops>(ord).colony_id;
      return k;
    }
    if (std::holds_alternative<LoadColonists>(ord)) {
      k.kind = CohortKind::Load;
      k.target_id = std::get<LoadColonists>(ord).colony_id;
      return k;
    }
    if (std::holds_alternative<UnloadColonists>(ord)) {
      k.kind = CohortKind::Unload;
      k.target_id = std::get<UnloadColonists>(ord).colony_id;
      return k;
    }
    if (std::holds_alternative<TransferCargoToShip>(ord)) {
      k.kind = CohortKind::Transfer;
      k.target_id = std::get<TransferCargoToShip>(ord).target_ship_id;
      return k;
    }
    if (std::holds_alternative<TransferFuelToShip>(ord)) {
      k.kind = CohortKind::Transfer;
      k.target_id = std::get<TransferFuelToShip>(ord).target_ship_id;
      return k;
    }
    if (std::holds_alternative<TransferTroopsToShip>(ord)) {
      k.kind = CohortKind::Transfer;
      k.target_id = std::get<TransferTroopsToShip>(ord).target_ship_id;
      return k;
    }
    if (std::holds_alternative<TransferColonistsToShip>(ord)) {
      k.kind = CohortKind::Transfer;
      k.target_id = std::get<TransferColonistsToShip>(ord).target_ship_id;
      return k;
    }
    if (std::holds_alternative<SalvageWreck>(ord)) {
      k.kind = CohortKind::Transfer;
      k.target_id = std::get<SalvageWreck>(ord).wreck_id;
      return k;
    }
    if (std::holds_alternative<SalvageWreckLoop>(ord)) {
      const auto& o = std::get<SalvageWreckLoop>(ord);
      k.kind = CohortKind::Transfer;
      k.target_id = (o.mode == 1 && o.dropoff_colony_id != kInvalidId) ? o.dropoff_colony_id : o.wreck_id;
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
      } else if (can_refill_from_repeat(so)) {
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
        const auto p = compute_power_allocation(*sd, sh->power_policy);
        if (!p.engines_online) base_speed_km_s = 0.0;
      }

      base_speed_km_s *= maintenance_speed_mult(*sh);
      base_speed_km_s *= ship_heat_speed_multiplier(*sh);
      base_speed_km_s *= ship_subsystem_engine_multiplier(*sh);

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
      } else if (can_refill_from_repeat(so)) {
        ord_ptr = &so.repeat_template.front();
      } else {
        continue;
      }

      const Order& ord = *ord_ptr;
      Id jump_id = kInvalidId;
      if (std::holds_alternative<TravelViaJump>(ord)) {
        jump_id = std::get<TravelViaJump>(ord).jump_point_id;
      } else if (std::holds_alternative<EscortShip>(ord)) {
        const auto& eo = std::get<EscortShip>(ord);
        const auto* tgt = find_ptr(state_.ships, eo.target_ship_id);
        if (!tgt) continue;
        if (tgt->system_id == sh->system_id) continue;
        const auto plan = plan_jump_route_for_ship_to_pos(ship_id, tgt->system_id, tgt->position_mkm,
                                                          eo.restrict_to_discovered, /*include_queued_jumps=*/false);
        if (!plan || plan->jump_ids.empty()) continue;
        jump_id = plan->jump_ids.front();
      } else {
        continue;
      }

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
      } else if (can_refill_from_repeat(so)) {
        ord_ptr = &so.repeat_template.front();
      } else {
        continue;
      }

      const Order& ord = *ord_ptr;
      if (std::holds_alternative<WaitDays>(ord)) continue;

      const auto key_opt = make_cohort_key(it_fleet->second, sh->system_id, ord);
      if (!key_opt) continue;
      const CohortKey key = *key_opt;
      if (key.kind != CohortKind::MovePoint && key.kind != CohortKind::Attack && key.kind != CohortKind::Escort)
        continue;

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
          } else if (can_refill_from_repeat(so)) {
              lord_ptr = &so.repeat_template.front();
            }
          }
          if (lord_ptr && std::holds_alternative<AttackShip>(*lord_ptr)) {
            const auto& ao = std::get<AttackShip>(*lord_ptr);
            if (ao.has_last_known) raw_target = ao.last_known_position_mkm;
          }
        }
      }

      // Shared formation solver (used by UI previews as well).
      std::unordered_map<Id, Vec2> member_pos;
      member_pos.reserve(members.size() * 2);
      for (Id sid : members) {
        if (const auto* sh = find_ptr(state_.ships, sid)) {
          member_pos.emplace(sid, sh->position_mkm);
        }
      }

      const auto offsets = compute_fleet_formation_offsets(fl->formation, spacing, leader_id, leader_pos, raw_target,
                                                           members, &member_pos);
      for (const auto& [sid, off] : offsets) {
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

    // --- Auto-retreat: resume suspended orders ---
    if (so.suspended && so.queue.empty()) {
      const double hp_frac = ship_hp_fraction(ship);
      const double resume_frac = std::clamp(ship.combat_doctrine.retreat_hp_resume_fraction, 0.0, 1.0);
      bool safe_here = true;
      if (ship.faction_id != kInvalidId && ship.system_id != kInvalidId) {
        safe_here = detected_hostile_ships_in_system(ship.faction_id, ship.system_id).empty();
      }

      if (safe_here && hp_frac + 1e-9 >= resume_frac) {
        so.queue = std::move(so.suspended_queue);
        so.repeat = so.suspended_repeat;
        so.repeat_count_remaining = so.suspended_repeat_count_remaining;
        so.repeat_template = std::move(so.suspended_repeat_template);

        so.suspended = false;
        so.suspended_queue.clear();
        so.suspended_repeat = false;
        so.suspended_repeat_count_remaining = 0;
        so.suspended_repeat_template.clear();

        if (is_player_faction(ship.faction_id)) {
          EventContext ctx;
          ctx.faction_id = ship.faction_id;
          ctx.system_id = ship.system_id;
          ctx.ship_id = ship_id;
          push_event(EventLevel::Info, EventCategory::Combat,
                     std::string("Orders resumed after retreat: ") + ship.name, ctx);
        }
      }
    }

    if (!so.suspended && so.queue.empty() && so.repeat && !so.repeat_template.empty()) {
      if (so.repeat_count_remaining == 0) {
        // Finite-repeat cycle complete: stop repeating (but keep the template).
        so.repeat = false;
      } else {
        so.queue = so.repeat_template;
        if (so.repeat_count_remaining > 0) {
          so.repeat_count_remaining -= 1;
        }
      }
    }

    // --- Auto-retreat: trigger emergency plan (may run even when queue is empty) ---
    if (!so.suspended && ship.combat_doctrine.auto_retreat && ship.faction_id != kInvalidId &&
        ship.system_id != kInvalidId) {
      const double hp_frac = ship_hp_fraction(ship);
      const double trig_hp = std::clamp(ship.combat_doctrine.retreat_hp_trigger_fraction, 0.0, 1.0);
      bool trigger = hp_frac <= trig_hp + 1e-9;

      if (!trigger && ship.combat_doctrine.retreat_when_out_of_missiles) {
        const double ammo_frac = missile_ammo_fraction(ship);
        const double trig_ammo =
            std::clamp(ship.combat_doctrine.retreat_missile_ammo_trigger_fraction, 0.0, 1.0);
        trigger = ammo_frac <= trig_ammo + 1e-9;
      }

      if (trigger) {
        const auto hostiles = detected_hostile_ships_in_system(ship.faction_id, ship.system_id);
        if (!hostiles.empty()) {
          // Suspend current orders & repeat state.
          so.suspended = true;
          so.suspended_queue = std::move(so.queue);
          so.suspended_repeat = so.repeat;
          so.suspended_repeat_count_remaining = so.repeat_count_remaining;
          so.suspended_repeat_template = std::move(so.repeat_template);

          // Disable repeat while retreating.
          so.repeat = false;
          so.repeat_count_remaining = 0;
          so.repeat_template.clear();

          // Build emergency retreat plan.
          so.queue = build_emergency_retreat_plan(ship, hostiles);

          if (so.queue.empty()) {
            // If planning failed for some reason, restore immediately.
            so.queue = std::move(so.suspended_queue);
            so.repeat = so.suspended_repeat;
            so.repeat_count_remaining = so.suspended_repeat_count_remaining;
            so.repeat_template = std::move(so.suspended_repeat_template);
            so.suspended = false;
            so.suspended_queue.clear();
            so.suspended_repeat = false;
            so.suspended_repeat_count_remaining = 0;
            so.suspended_repeat_template.clear();
          } else if (is_player_faction(ship.faction_id)) {
            EventContext ctx;
            ctx.faction_id = ship.faction_id;
            ctx.system_id = ship.system_id;
            ctx.ship_id = ship_id;

            std::string reason;
            if (hp_frac <= trig_hp + 1e-9) {
              reason = std::string("HP ") +
                       std::to_string(static_cast<int>(std::lround(hp_frac * 100.0))) + "%";
            } else {
              const double ammo_frac = missile_ammo_fraction(ship);
              reason = std::string("Missile ammo ") +
                       std::to_string(static_cast<int>(std::lround(ammo_frac * 100.0))) + "%";
            }

            push_event(EventLevel::Warn, EventCategory::Combat,
                       std::string("Emergency retreat: ") + ship.name + " (" + reason + ")", ctx);
          }
        }
      }
    }

    auto& q = so.queue;
    if (q.empty()) continue;

    if (std::holds_alternative<WaitDays>(q.front())) {
      auto& ord = std::get<WaitDays>(q.front());
      if (ord.days_remaining <= 0) {
        q.erase(q.begin());
        continue;
      }
      // Accumulate fractional days so sub-day ticks don't consume a full day.
      ord.progress_days = std::max(0.0, ord.progress_days) + dt_days;
      while (ord.days_remaining > 0 && ord.progress_days >= 1.0 - 1e-12) {
        ord.days_remaining -= 1;
        ord.progress_days -= 1.0;
      }
      if (ord.days_remaining <= 0) q.erase(q.begin());
      continue;
    }

    Vec2 target = ship.position_mkm;
    double desired_range = 0.0; 
    bool attack_has_contact = false;

    // Escort ops (follow another friendly ship; can jump across systems).
    bool is_escort_op = false;

    // Jump survey ops (stay at a jump point until it is surveyed).
    bool is_survey_jump_op = false;
    SurveyJumpPoint* survey_jump_ord = nullptr;
    Id survey_jump_id = kInvalidId;
    bool survey_transit_when_done = false;

    bool escort_is_jump_leg = false;
    Id escort_jump_id = kInvalidId;

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

    // Salvage ops (wreck -> ship cargo)
    bool is_salvage_op = false;
    SalvageWreck* salvage_ord = nullptr;
    Id salvage_wreck_id = kInvalidId;
    std::string salvage_mineral;
    double salvage_tons = 0.0;

    // Salvage loop ops (wreck <-> friendly colony)
    bool is_salvage_loop_op = false;

    // Anomaly investigation ops (anomaly -> research reward / component unlock).
    bool is_investigate_anomaly_op = false;
    InvestigateAnomaly* investigate_anom_ord = nullptr;
    Id investigate_anom_id = kInvalidId;

    // Mobile mining ops (body -> ship cargo)
    bool is_mining_op = false;
    MineBody* mine_ord = nullptr;
    Id mine_body_id = kInvalidId;
    std::string mine_mineral;
    bool mine_stop_when_full = true;

    // Fuel transfer ops (ship-to-ship refueling)
    bool is_fuel_transfer_op = false;
    TransferFuelToShip* fuel_transfer_ord = nullptr;
    Id fuel_target_ship_id = kInvalidId;
    double fuel_tons = 0.0;

    // Troop transfer ops (ship-to-ship troop movement)
    bool is_troop_transfer_op = false;
    TransferTroopsToShip* troop_transfer_ord = nullptr;
    Id troop_target_ship_id = kInvalidId;
    double troop_transfer_strength = 0.0;

    // Colonist transfer ops (ship-to-ship passenger movement)
    bool is_colonist_transfer_op = false;
    TransferColonistsToShip* colonist_transfer_ord = nullptr;
    Id colonist_target_ship_id = kInvalidId;
    double colonist_transfer_millions = 0.0;

    // Troop ops
    bool is_troop_op = false;
    // 0=LoadTroops, 1=UnloadTroops, 2=Invade
    int troop_mode = 0;
    LoadTroops* load_troops_ord = nullptr;
    UnloadTroops* unload_troops_ord = nullptr;
    Id troop_colony_id = kInvalidId;
    double troop_strength = 0.0;

    // Colonist ops (population transport)
    bool is_colonist_op = false;
    // 0=LoadColonists, 1=UnloadColonists
    int colonist_mode = 0;
    LoadColonists* load_colonists_ord = nullptr;
    UnloadColonists* unload_colonists_ord = nullptr;
    Id colonist_colony_id = kInvalidId;
    double colonist_millions = 0.0;

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
    } else if (std::holds_alternative<SurveyJumpPoint>(q.front())) {
      auto& ord = std::get<SurveyJumpPoint>(q.front());
      is_survey_jump_op = true;
      survey_jump_ord = &ord;
      survey_jump_id = ord.jump_point_id;
      survey_transit_when_done = ord.transit_when_done;

      const auto* jp = find_ptr(state_.jump_points, survey_jump_id);
      if (!jp || jp->system_id != ship.system_id) {
        q.erase(q.begin());
        continue;
      }
      target = jp->position_mkm;
    } else if (std::holds_alternative<AttackShip>(q.front())) {
      auto& ord = std::get<AttackShip>(q.front());
      const Id target_id = ord.target_ship_id;
      const auto* tgt = find_ptr(state_.ships, target_id);
      if (!tgt) {
        // Target destroyed. Keep state-validation invariants by removing the order.
        q.erase(q.begin());
        continue;
      }

      if (tgt->faction_id == ship.faction_id) {
        // Target changed hands (captured) or is otherwise no longer hostile.
        q.erase(q.begin());
        continue;
      }

      // Do not allow offensive action while an active treaty exists between the factions.
      // This prevents ceasefires/non-aggression pacts/alliances/trade agreements from being
      // instantly broken by queued AttackShip orders.
      TreatyType tt = TreatyType::Ceasefire;
      if (strongest_active_treaty_between(state_, ship.faction_id, tgt->faction_id, &tt)) {
        std::ostringstream oss;
        oss << "Attack order cancelled due to active treaty between factions.";
        EventContext ctx;
        ctx.faction_id = ship.faction_id;
        ctx.faction_id2 = tgt->faction_id;
        ctx.ship_id = ship_id;
        ctx.system_id = ship.system_id;
        this->push_event(EventLevel::Warn, EventCategory::Diplomacy, oss.str(), ctx);
        q.erase(q.begin());
        continue;
      }

      const int now = static_cast<int>(state_.date.days_since_epoch());

      attack_has_contact = is_ship_detected_by_faction(ship.faction_id, target_id);
      // Be defensive: detection is only meaningful when both ships are in the same system.
      if (attack_has_contact && tgt->system_id != ship.system_id) attack_has_contact = false;

      // An explicit AttackShip order acts as a de-facto declaration of hostilities if needed.
      if (attack_has_contact && !are_factions_hostile(ship.faction_id, tgt->faction_id)) {
        set_diplomatic_status(ship.faction_id, tgt->faction_id, DiplomacyStatus::Hostile, /*reciprocal=*/true,
                             /*push_event_on_change=*/true);
      }


      if (attack_has_contact) {
        target = tgt->position_mkm;
        ord.last_known_position_mkm = target;
        ord.has_last_known = true;
        ord.last_known_system_id = ship.system_id;
        ord.last_known_day = now;
        ord.pursuit_hops = 0;
        // Reset lost-contact search state when we reacquire the target.
        ord.search_waypoint_index = 0;
        ord.has_search_offset = false;
        ord.search_offset_mkm = Vec2{0.0, 0.0};
        const auto* d = find_design(ship.design_id);
        const double beam_range = d ? std::max(0.0, d->weapon_range_mkm) : 0.0;
        const double missile_range = d ? std::max(0.0, d->missile_range_mkm) : 0.0;

        auto select_range = [&](EngagementRangeMode mode) -> double {
          switch (mode) {
            case EngagementRangeMode::Beam: return beam_range;
            case EngagementRangeMode::Missile: return missile_range;
            case EngagementRangeMode::Max: return std::max(beam_range, missile_range);
            case EngagementRangeMode::Min: {
              double r = 0.0;
              if (beam_range > 1e-9) r = beam_range;
              if (missile_range > 1e-9) r = (r > 1e-9) ? std::min(r, missile_range) : missile_range;
              return r;
            }
            case EngagementRangeMode::Custom: return std::max(0.0, ship.combat_doctrine.custom_range_mkm);
            case EngagementRangeMode::Auto:
            default:
              return (beam_range > 1e-9) ? beam_range : ((missile_range > 1e-9) ? missile_range : 0.0);
          }
        };

        const auto& doc = ship.combat_doctrine;
        const double base_range = select_range(doc.range_mode);
        const double frac = std::clamp(doc.range_fraction, 0.0, 1.0);
        const double min_r = std::max(0.0, doc.min_range_mkm);
        double dr = base_range * frac;
        if (base_range <= 1e-9) dr = min_r;
        desired_range = std::max(dr, min_r);
        if (!std::isfinite(desired_range)) desired_range = 0.1;
        // If the target is disabled and we have troops, close to boarding range.
        if (cfg_.enable_boarding && ship.troops >= cfg_.boarding_min_attacker_troops) {
          const auto* td = find_design(tgt->design_id);
          const double tmax_hp = (td && td->max_hp > 1e-9) ? td->max_hp : std::max(1.0, tgt->hp);
          const double hp_frac = (tmax_hp > 1e-9) ? (tgt->hp / tmax_hp) : 1.0;
          const bool shields_ok = (!cfg_.boarding_require_shields_down) || (tgt->shields <= 1e-9);
          if (shields_ok && hp_frac <= cfg_.boarding_target_hp_fraction) {
            desired_range = std::min(desired_range, std::max(0.0, cfg_.boarding_range_mkm));
          }
        }

        // Lead pursuit: if we have a simple velocity estimate for this contact,
        // aim at an intercept point (to desired_range) rather than tail-chasing
        // the instantaneous position.
        if (const auto* fac = find_ptr(state_.factions, ship.faction_id)) {
          if (auto contact_it = fac->ship_contacts.find(target_id); contact_it != fac->ship_contacts.end()) {
            const auto& c = contact_it->second;
            if (c.system_id == ship.system_id && c.prev_seen_day >= 0 && c.prev_seen_day < c.last_seen_day) {
              const int dt = c.last_seen_day - c.prev_seen_day;
              if (dt > 0) {
                const Vec2 v_mkm_per_day =
                    (c.last_seen_position_mkm - c.prev_seen_position_mkm) * (1.0 / static_cast<double>(dt));
                if (std::isfinite(v_mkm_per_day.x) && std::isfinite(v_mkm_per_day.y) &&
                    (std::abs(v_mkm_per_day.x) > 1e-9 || std::abs(v_mkm_per_day.y) > 1e-9)) {
                  const double sp_mkm_per_day = mkm_per_day_from_speed(ship.speed_km_s, cfg_.seconds_per_day);
                  if (sp_mkm_per_day > 1e-12) {
                    const int lead_cap_i = std::max(0, std::min(cfg_.contact_prediction_max_days, 14));
                    const auto aim = compute_intercept_aim(ship.position_mkm, sp_mkm_per_day, target, v_mkm_per_day,
                                                          desired_range, static_cast<double>(lead_cap_i));
                    target = aim.aim_position_mkm;
                  }
                }
              }
            }
          }
        }
      } else {
        // Seed missing tracking metadata for backward compatibility with older
        // saves/templates that only stored a position.
        if (ord.has_last_known) {
          if (ord.last_known_system_id == kInvalidId) ord.last_known_system_id = ship.system_id;
          if (ord.last_known_day == 0) ord.last_known_day = now;
        }

        if (!ord.has_last_known) {
          q.erase(q.begin());
          continue;
        }

        // Give up on pursuit when the last reliable sighting is too stale.
        // (This mirrors how contact prediction is bounded, and prevents
        //  AttackShip from roaming indefinitely on dead leads.)
        if (cfg_.contact_prediction_max_days > 0) {
          const int age = std::max(0, now - ord.last_known_day);
          if (age > cfg_.contact_prediction_max_days) {
            q.erase(q.begin());
            continue;
          }
        }

        // If the ship is not currently in the system where the last-known
        // position is defined, route back there (unrestricted).
        if (ord.last_known_system_id != kInvalidId && ship.system_id != ord.last_known_system_id) {
          const auto plan = plan_jump_route_for_ship_to_pos(ship_id, ord.last_known_system_id,
                                                          ord.last_known_position_mkm,
                                                          /*restrict_to_discovered=*/false,
                                                          /*include_queued_jumps=*/false);
          if (plan && !plan->jump_ids.empty()) {
            AttackShip next = ord;
            std::vector<Order> prefix;
            prefix.reserve(plan->jump_ids.size() + 1);
            for (Id jid : plan->jump_ids) prefix.push_back(TravelViaJump{jid});
            prefix.push_back(next);

            q.erase(q.begin());
            q.insert(q.begin(), prefix.begin(), prefix.end());
            continue;
          }
          // No route available; hold position but keep the order.
          target = ship.position_mkm;
          desired_range = 0.0;
          continue;
        }

        // Keep extrapolating last_known_position while the contact is lost,
        // so attackers continue to chase the track instead of beelining to a
        // stale point forever.
        Vec2 track_v_mkm_per_day{0.0, 0.0};
        bool has_track_v = false;
        int track_age_days = 0;
        const Contact* track_contact = nullptr;
        if (const auto* fac = find_ptr(state_.factions, ship.faction_id)) {
          if (auto contact_it = fac->ship_contacts.find(target_id); contact_it != fac->ship_contacts.end()) {
            // Only use contact track data when it's in the same coordinate
            // frame. (AttackShip can pursue hypothesized jump transits across
            // systems, so the target ship's true system may differ.)
            if (contact_it->second.system_id == ship.system_id) {
              const auto pred = predict_contact_position(contact_it->second, now, cfg_.contact_prediction_max_days);
              ord.last_known_position_mkm = pred.predicted_position_mkm;
              ord.last_known_system_id = contact_it->second.system_id;
              ord.last_known_day = contact_it->second.last_seen_day;

              track_v_mkm_per_day = pred.velocity_mkm_per_day;
              has_track_v = pred.has_velocity &&
                            (std::abs(track_v_mkm_per_day.x) > 1e-9 || std::abs(track_v_mkm_per_day.y) > 1e-9);
              track_age_days = pred.age_days;
              track_contact = &contact_it->second;
            }
          }
        }

        // Jump-chase heuristic: if the last seen position was essentially on a
        // jump point, and we lost contact recently, follow the same jump.
        //
        // This enables cross-system pursuit without omniscient knowledge of
        // the target ship's current location.
        if (track_contact && track_contact->system_id == ship.system_id) {
          const int contact_age = std::max(0, now - track_contact->last_seen_day);
          const int hop_limit = 4;
          if (contact_age <= 1 && ord.pursuit_hops < hop_limit) {
            const auto* sys = find_ptr(state_.systems, ship.system_id);
            if (sys) {
              Id best_jump_id = kInvalidId;
              double best_dist = 1e300;
              for (Id jid : sys->jump_points) {
                if (jid == kInvalidId) continue;
                const auto* jp = find_ptr(state_.jump_points, jid);
                if (!jp) continue;
                if (jp->linked_jump_id == kInvalidId) continue;
                const double d = (jp->position_mkm - track_contact->last_seen_position_mkm).length();
                if (d < best_dist) {
                  best_dist = d;
                  best_jump_id = jid;
                }
              }

              // Require the contact to have been essentially on the jump point.
              const double thresh = std::max(0.0, dock_range) * 1.25;
              if (best_jump_id != kInvalidId && best_dist <= thresh + 1e-9) {
                const auto* jp = find_ptr(state_.jump_points, best_jump_id);
                const auto* dst = jp ? find_ptr(state_.jump_points, jp->linked_jump_id) : nullptr;
                if (jp && dst && dst->system_id != kInvalidId) {
                  AttackShip next = ord;
                  next.has_last_known = true;
                  next.last_known_system_id = dst->system_id;
                  next.last_known_position_mkm = dst->position_mkm;
                  next.last_known_day = now;
                  next.pursuit_hops = ord.pursuit_hops + 1;
                  // New coordinate frame: restart the search pattern.
                  next.search_waypoint_index = 0;
                  next.has_search_offset = false;
                  next.search_offset_mkm = Vec2{0.0, 0.0};

                  q.erase(q.begin());
                  q.insert(q.begin(), TravelViaJump{best_jump_id});
                  q.insert(q.begin() + 1, next);
                  continue;
                }
              }
            }
          }
        }

        target = ord.last_known_position_mkm;
        desired_range = 0.0;

        // Lead pursuit on stale tracks too (bounded by remaining prediction budget).
        if (has_track_v && std::isfinite(track_v_mkm_per_day.x) && std::isfinite(track_v_mkm_per_day.y)) {
          const double sp_mkm_per_day = mkm_per_day_from_speed(ship.speed_km_s, cfg_.seconds_per_day);
          if (sp_mkm_per_day > 1e-12) {
            const int remaining = std::max(0, cfg_.contact_prediction_max_days - track_age_days);
            const int lead_cap_i = std::max(0, std::min(remaining, 7));
            if (lead_cap_i > 0) {
              const auto aim = compute_intercept_aim(ship.position_mkm, sp_mkm_per_day, target, track_v_mkm_per_day,
                                                    /*desired_range_mkm=*/0.0, static_cast<double>(lead_cap_i));
              target = aim.aim_position_mkm;
            }
          }
        }

        // Lost-contact pursuit is a *bounded search* around the predicted track.
        //
        // Previously, this used a per-day pseudo-random offset which caused
        // visible "zig-zag" retargeting. We now keep a persistent waypoint
        // offset and advance it only after reaching each waypoint.
        const Vec2 search_center = target;

        auto reset_search_state = [&]() {
          ord.search_waypoint_index = 0;
          ord.has_search_offset = false;
          ord.search_offset_mkm = Vec2{0.0, 0.0};
        };

        if (!std::isfinite(ord.search_offset_mkm.x) || !std::isfinite(ord.search_offset_mkm.y)) {
          reset_search_state();
        }
        if (ord.search_waypoint_index < 0) ord.search_waypoint_index = 0;

        // Compute the uncertainty radius.
        double unc = 0.0;
        if (cfg_.enable_contact_uncertainty && cfg_.contact_search_offset_fraction > 1e-9) {
          if (track_contact && track_contact->system_id == ship.system_id) {
            unc = contact_uncertainty_radius_mkm(*track_contact, now);
          } else {
            // Fallback after jump-chasing: grow an uncertainty bubble based on
            // the pursuer's speed and the time since the last-known update.
            const int age = std::max(0, now - ord.last_known_day);
            const double sp = mkm_per_day_from_speed(ship.speed_km_s, cfg_.seconds_per_day);
            const double gmin = std::max(0.0, cfg_.contact_uncertainty_growth_min_mkm_per_day);
            const double gfrac = std::max(0.0, cfg_.contact_uncertainty_growth_fraction_of_speed);
            const double growth = std::max(gmin, gfrac * sp);
            unc = cfg_.contact_uncertainty_min_mkm + static_cast<double>(age) * growth;
            unc = std::clamp(unc, cfg_.contact_uncertainty_min_mkm, cfg_.contact_uncertainty_max_mkm);
          }
        }
        if (!std::isfinite(unc) || unc < 0.0) unc = 0.0;

        double rad = std::max(0.0, unc) * std::max(0.0, cfg_.contact_search_offset_fraction);
        if (!std::isfinite(rad) || rad < 0.0) rad = 0.0;

        // Optional cap: don't generate waypoints that are physically impossible
        // to reach before the track goes stale.
        if (rad > 1e-9 && cfg_.contact_search_radius_speed_cap_fraction > 1e-9 && cfg_.contact_prediction_max_days > 0) {
          const int age = std::max(0, now - ord.last_known_day);
          const int remaining = std::max(0, cfg_.contact_prediction_max_days - age);
          const double sp = mkm_per_day_from_speed(ship.speed_km_s, cfg_.seconds_per_day);
          const double cap = sp * static_cast<double>(remaining) * cfg_.contact_search_radius_speed_cap_fraction;
          if (std::isfinite(cap) && cap > 0.0) {
            rad = std::min(rad, cap);
          }
        }

        if (rad <= 1e-6) {
          // No meaningful uncertainty => just go to the predicted center.
          reset_search_state();
          target = search_center;
        } else {
          // Keep an existing offset within the current radius (in case the cap
          // shrinks over time).
          if (ord.has_search_offset) {
            const double len = ord.search_offset_mkm.length();
            if (len > rad && len > 1e-12) {
              ord.search_offset_mkm = ord.search_offset_mkm * (rad / len);
            }
          }

          auto current_waypoint = [&]() {
            return search_center + (ord.has_search_offset ? ord.search_offset_mkm : Vec2{0.0, 0.0});
          };

          const Vec2 wp = current_waypoint();
          const double wp_dist = (ship.position_mkm - wp).length();

          // Advance to the next waypoint only after reaching the current one.
          if (wp_dist <= arrive_eps + 1e-9) {
            ord.search_waypoint_index = std::max(0, ord.search_waypoint_index) + 1;
            const int pts = std::max(8, cfg_.contact_search_pattern_points);
            const double seed_ang = contact_search_seed_angle_rad(ship_id, target_id);
            ord.search_offset_mkm = contact_search_spiral_offset_mkm(ord.search_waypoint_index, pts, rad, seed_ang);
            ord.has_search_offset = (ord.search_waypoint_index > 0);
          } else {
            // Backward compatibility: if an older save had an index but no offset,
            // rebuild it deterministically.
            if (ord.search_waypoint_index > 0 && !ord.has_search_offset) {
              const int pts = std::max(8, cfg_.contact_search_pattern_points);
              const double seed_ang = contact_search_seed_angle_rad(ship_id, target_id);
              ord.search_offset_mkm = contact_search_spiral_offset_mkm(ord.search_waypoint_index, pts, rad, seed_ang);
              ord.has_search_offset = true;
            }
          }

          target = current_waypoint();
        }
      }
    } else if (std::holds_alternative<EscortShip>(q.front())) {
      auto& ord = std::get<EscortShip>(q.front());
      is_escort_op = true;

      const Id target_id = ord.target_ship_id;
      const auto* tgt = find_ptr(state_.ships, target_id);
      if (!tgt || target_id == ship_id) {
        q.erase(q.begin());
        continue;
      }

      // By default, escorts only apply to mutual-friendly ships. Some
      // contract-driven cases (e.g. civilian convoys) allow escorting neutral
      // targets as long as neither side is Hostile.
      if (!ord.allow_neutral) {
        if (!are_factions_mutual_friendly(ship.faction_id, tgt->faction_id)) {
          q.erase(q.begin());
          continue;
        }
      } else {
        if (are_factions_hostile(ship.faction_id, tgt->faction_id) ||
            are_factions_hostile(tgt->faction_id, ship.faction_id)) {
          q.erase(q.begin());
          continue;
        }
      }

      const double follow_mkm = std::max(0.0, ord.follow_distance_mkm);
      desired_range = (follow_mkm > 1e-12) ? follow_mkm : dock_range;

      if (tgt->system_id == ship.system_id) {
        target = tgt->position_mkm;
      } else {
        const auto plan = plan_jump_route_for_ship_to_pos(ship_id, tgt->system_id, tgt->position_mkm,
                                                          ord.restrict_to_discovered,
                                                          /*include_queued_jumps=*/false);
        if (!plan || plan->jump_ids.empty()) {
          // No route available (under fog-of-war restrictions or disconnected jump graph).
          target = ship.position_mkm;
          desired_range = 0.0;
        } else {
          escort_jump_id = plan->jump_ids.front();
          const auto* jp = find_ptr(state_.jump_points, escort_jump_id);
          if (!jp || jp->system_id != ship.system_id) {
            target = ship.position_mkm;
            desired_range = 0.0;
          } else {
            escort_is_jump_leg = true;
            target = jp->position_mkm;
            desired_range = 0.0;
          }
        }
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
      const bool trade_ok = [&]() {
        if (!colony) return false;
        if (are_factions_trade_partners(ship.faction_id, colony->faction_id)) return true;

        // Allow Merchant Guild civilian convoys to trade with non-hostile colonies when enabled.
        if (allow_civilian_trade_cargo_ops && ship.faction_id == merchant_faction_id) {
          return !are_factions_hostile(ship.faction_id, colony->faction_id) &&
                 !are_factions_hostile(colony->faction_id, ship.faction_id);
        }
        return false;
      }();
      if (!trade_ok) { q.erase(q.begin()); continue; }
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
      const bool trade_ok = [&]() {
        if (!colony) return false;
        if (are_factions_trade_partners(ship.faction_id, colony->faction_id)) return true;

        // Allow Merchant Guild civilian convoys to trade with non-hostile colonies when enabled.
        if (allow_civilian_trade_cargo_ops && ship.faction_id == merchant_faction_id) {
          return !are_factions_hostile(ship.faction_id, colony->faction_id) &&
                 !are_factions_hostile(colony->faction_id, ship.faction_id);
        }
        return false;
      }();
      if (!trade_ok) { q.erase(q.begin()); continue; }
      const auto* body = find_ptr(state_.bodies, colony->body_id);
      if (!body || body->system_id != ship.system_id) { q.erase(q.begin()); continue; }
      target = body->position_mkm;
    } else if (std::holds_alternative<MineBody>(q.front())) {
      auto& ord = std::get<MineBody>(q.front());
      is_mining_op = true;
      mine_ord = &ord;
      mine_body_id = ord.body_id;
      mine_mineral = ord.mineral;
      mine_stop_when_full = ord.stop_when_cargo_full;

      const auto* body = find_ptr(state_.bodies, mine_body_id);
      if (!body || body->system_id != ship.system_id) { q.erase(q.begin()); continue; }

      const auto* d = find_design(ship.design_id);
      if (!d || d->mining_tons_per_day <= 0.0 || d->cargo_tons <= 0.0) { q.erase(q.begin()); continue; }

      target = body->position_mkm;
    } else if (std::holds_alternative<LoadTroops>(q.front())) {
      auto& ord = std::get<LoadTroops>(q.front());
      is_troop_op = true;
      troop_mode = 0;
      load_troops_ord = &ord;
      troop_colony_id = ord.colony_id;
      troop_strength = ord.strength;
      const auto* colony = find_ptr(state_.colonies, troop_colony_id);
      if (!colony || colony->faction_id != ship.faction_id) { q.erase(q.begin()); continue; }
      const auto* body = find_ptr(state_.bodies, colony->body_id);
      if (!body || body->system_id != ship.system_id) { q.erase(q.begin()); continue; }
      target = body->position_mkm;
    } else if (std::holds_alternative<UnloadTroops>(q.front())) {
      auto& ord = std::get<UnloadTroops>(q.front());
      is_troop_op = true;
      troop_mode = 1;
      unload_troops_ord = &ord;
      troop_colony_id = ord.colony_id;
      troop_strength = ord.strength;
      const auto* colony = find_ptr(state_.colonies, troop_colony_id);
      if (!colony || colony->faction_id != ship.faction_id) { q.erase(q.begin()); continue; }
      const auto* body = find_ptr(state_.bodies, colony->body_id);
      if (!body || body->system_id != ship.system_id) { q.erase(q.begin()); continue; }
      target = body->position_mkm;
    } else if (std::holds_alternative<LoadColonists>(q.front())) {
      auto& ord = std::get<LoadColonists>(q.front());
      is_colonist_op = true;
      colonist_mode = 0;
      load_colonists_ord = &ord;
      colonist_colony_id = ord.colony_id;
      colonist_millions = ord.millions;

      const auto* colony = find_ptr(state_.colonies, colonist_colony_id);
      if (!colony || colony->faction_id != ship.faction_id) { q.erase(q.begin()); continue; }
      const auto* d = find_design(ship.design_id);
      if (!d || d->colony_capacity_millions <= 0.0) { q.erase(q.begin()); continue; }
      const auto* body = find_ptr(state_.bodies, colony->body_id);
      if (!body || body->system_id != ship.system_id) { q.erase(q.begin()); continue; }
      target = body->position_mkm;
    } else if (std::holds_alternative<UnloadColonists>(q.front())) {
      auto& ord = std::get<UnloadColonists>(q.front());
      is_colonist_op = true;
      colonist_mode = 1;
      unload_colonists_ord = &ord;
      colonist_colony_id = ord.colony_id;
      colonist_millions = ord.millions;

      const auto* colony = find_ptr(state_.colonies, colonist_colony_id);
      if (!colony || colony->faction_id != ship.faction_id) { q.erase(q.begin()); continue; }
      const auto* d = find_design(ship.design_id);
      if (!d || d->colony_capacity_millions <= 0.0) { q.erase(q.begin()); continue; }
      const auto* body = find_ptr(state_.bodies, colony->body_id);
      if (!body || body->system_id != ship.system_id) { q.erase(q.begin()); continue; }
      target = body->position_mkm;
    } else if (std::holds_alternative<InvadeColony>(q.front())) {
      auto& ord = std::get<InvadeColony>(q.front());
      is_troop_op = true;
      troop_colony_id = ord.colony_id;

      const auto* colony = find_ptr(state_.colonies, troop_colony_id);
      if (!colony) { q.erase(q.begin()); continue; }

      const auto* body = find_ptr(state_.bodies, colony->body_id);
      if (!body || body->system_id != ship.system_id) { q.erase(q.begin()); continue; }

      const bool target_is_friendly = (colony->faction_id == ship.faction_id);

      // If the colony already belongs to us (e.g. capture happened earlier this tick),
      // treat the invasion order as an unload-to-garrison operation so transports don't
      // cancel and keep their troops embarked.
      if (target_is_friendly) {
        troop_mode = 1;
        troop_strength = 0.0; // unload as much as possible
        target = body->position_mkm;
      } else {
        troop_mode = 2;

        // An explicit invasion is an act of hostility, but an active treaty requires an explicit
        // diplomatic break (cancel treaty / declare war) first.
        TreatyType tt = TreatyType::Ceasefire;
        if (strongest_active_treaty_between(state_, ship.faction_id, colony->faction_id, &tt)) {
          std::ostringstream oss;
          oss << "Invasion order cancelled due to active treaty between factions.";
          EventContext ctx;
          ctx.faction_id = ship.faction_id;
          ctx.faction_id2 = colony->faction_id;
          ctx.ship_id = ship_id;
          ctx.colony_id = troop_colony_id;
          ctx.system_id = ship.system_id;
          this->push_event(EventLevel::Warn, EventCategory::Diplomacy, oss.str(), ctx);
          q.erase(q.begin());
          continue;
        }

        // An explicit invasion is an act of hostility.
        if (!are_factions_hostile(ship.faction_id, colony->faction_id)) {
          set_diplomatic_status(ship.faction_id, colony->faction_id, DiplomacyStatus::Hostile, /*reciprocal=*/true,
                               /*push_event_on_change=*/true);
        }
        target = body->position_mkm;
      }
    } else if (std::holds_alternative<BombardColony>(q.front())) {
      auto& ord = std::get<BombardColony>(q.front());
      const auto* colony = find_ptr(state_.colonies, ord.colony_id);
      if (!colony) { q.erase(q.begin()); continue; }
      if (colony->faction_id == ship.faction_id) { q.erase(q.begin()); continue; }
      const auto* body = find_ptr(state_.bodies, colony->body_id);
      if (!body || body->system_id != ship.system_id) { q.erase(q.begin()); continue; }

      const auto* d = find_design(ship.design_id);
      if (!d || d->weapon_damage <= 0.0 || d->weapon_range_mkm <= 0.0) { q.erase(q.begin()); continue; }

      // Bombardment is an explicit act of hostility, but an active treaty requires an explicit
      // diplomatic break (cancel treaty / declare war) first.
      TreatyType tt = TreatyType::Ceasefire;
      if (strongest_active_treaty_between(state_, ship.faction_id, colony->faction_id, &tt)) {
        std::ostringstream oss;
        oss << "Bombardment order cancelled due to active treaty between factions.";
        EventContext ctx;
        ctx.faction_id = ship.faction_id;
        ctx.faction_id2 = colony->faction_id;
        ctx.ship_id = ship_id;
        ctx.colony_id = ord.colony_id;
        ctx.system_id = ship.system_id;
        this->push_event(EventLevel::Warn, EventCategory::Diplomacy, oss.str(), ctx);
        q.erase(q.begin());
        continue;
      }

      // Bombardment is an explicit act of hostility.
      if (!are_factions_hostile(ship.faction_id, colony->faction_id)) {
        set_diplomatic_status(ship.faction_id, colony->faction_id, DiplomacyStatus::Hostile, /*reciprocal=*/true,
                             /*push_event_on_change=*/true);
      }

      target = body->position_mkm;
      const double frac = std::clamp(cfg_.bombard_standoff_range_fraction, 0.0, 1.0);
      desired_range = std::max(0.0, d->weapon_range_mkm * frac);
    } else if (std::holds_alternative<SalvageWreck>(q.front())) {
      auto& ord = std::get<SalvageWreck>(q.front());
      is_salvage_op = true;
      salvage_ord = &ord;
      salvage_wreck_id = ord.wreck_id;
      salvage_mineral = ord.mineral;
      salvage_tons = ord.tons;

      const auto* w = find_ptr(state_.wrecks, salvage_wreck_id);
      if (!w || w->system_id != ship.system_id) { q.erase(q.begin()); continue; }
      target = w->position_mkm;
    } else if (std::holds_alternative<SalvageWreckLoop>(q.front())) {
      is_salvage_loop_op = true;

      auto ord = std::get<SalvageWreckLoop>(q.front()); // copy (we may rewrite it)
      if (ord.mode != 0 && ord.mode != 1) ord.mode = 0;

      auto is_valid_dropoff = [&](Id colony_id) {
        if (colony_id == kInvalidId) return false;
        const auto* col = find_ptr(state_.colonies, colony_id);
        if (!col) return false;
        if (col->faction_id != ship.faction_id) return false;
        const auto* body = find_ptr(state_.bodies, col->body_id);
        if (!body) return false;
        if (body->system_id == kInvalidId) return false;
        return true;
      };

      auto pick_best_dropoff = [&]() -> Id {
        if (ship.system_id == kInvalidId) return kInvalidId;
        Id best = kInvalidId;
        double best_eta = std::numeric_limits<double>::infinity();
        for (const auto& [cid, col] : state_.colonies) {
          if (col.faction_id != ship.faction_id) continue;
          const auto* body = find_ptr(state_.bodies, col.body_id);
          if (!body || body->system_id == kInvalidId) continue;
          const auto plan = plan_jump_route_cached(ship.system_id, ship.position_mkm, ship.faction_id, ship.speed_km_s,
                                                   body->system_id, ord.restrict_to_discovered, body->position_mkm);
          if (!plan) continue;
          const double eta = std::isfinite(plan->total_eta_days) ? plan->total_eta_days : plan->total_distance_mkm;
          if (eta < best_eta) {
            best_eta = eta;
            best = cid;
          }
        }
        return best;
      };

      auto route_to_system_or_cancel = [&](Id target_system_id, const Vec2& goal_pos_mkm) -> bool {
        if (target_system_id == kInvalidId || ship.system_id == kInvalidId) return false;
        if (target_system_id == ship.system_id) return true;

        const auto plan = plan_jump_route_cached(ship.system_id, ship.position_mkm, ship.faction_id, ship.speed_km_s,
                                                 target_system_id, ord.restrict_to_discovered, goal_pos_mkm);
        if (!plan || plan->jump_ids.empty()) return false;

        std::vector<Order> legs;
        legs.reserve(plan->jump_ids.size());
        for (Id jid : plan->jump_ids) legs.emplace_back(TravelViaJump{jid});

        q.erase(q.begin());
        q.insert(q.begin(), legs.begin(), legs.end());
        q.insert(q.begin() + legs.size(), ord);
        return true; // queued travel legs
      };

      if (ord.mode == 0) {
        const auto* w = find_ptr(state_.wrecks, ord.wreck_id);
        if (!w) {
          q.erase(q.begin());
          continue;
        }

        if (w->system_id != ship.system_id) {
          if (!route_to_system_or_cancel(w->system_id, w->position_mkm)) {
            // No route; drop the order.
            // (We expect issue_salvage_wreck_loop to have queued travel legs already.)
            q.erase(q.begin());
          }
          continue;
        }

        target = w->position_mkm;

        // Configure salvage transfer helpers.
        salvage_wreck_id = ord.wreck_id;
        salvage_mineral.clear();
        salvage_tons = 0.0;
      } else {
        if (!is_valid_dropoff(ord.dropoff_colony_id)) {
          ord.dropoff_colony_id = pick_best_dropoff();
        }
        if (!is_valid_dropoff(ord.dropoff_colony_id)) {
          q.erase(q.begin());
          continue;
        }

        const auto* col = find_ptr(state_.colonies, ord.dropoff_colony_id);
        const auto* body = col ? find_ptr(state_.bodies, col->body_id) : nullptr;
        if (!body) {
          q.erase(q.begin());
          continue;
        }

        if (body->system_id != ship.system_id) {
          if (!route_to_system_or_cancel(body->system_id, body->position_mkm)) {
            q.erase(q.begin());
          }
          continue;
        }

        target = body->position_mkm;

        // Configure cargo transfer helpers (unload all minerals).
        cargo_mode = 1;
        cargo_colony_id = ord.dropoff_colony_id;
        cargo_mineral.clear();
        cargo_tons = 0.0;
      }

      // Persist any fix-ups (mode clamp / selected dropoff).
      std::get<SalvageWreckLoop>(q.front()) = ord;
    } else if (std::holds_alternative<InvestigateAnomaly>(q.front())) {
      auto& ord = std::get<InvestigateAnomaly>(q.front());
      is_investigate_anomaly_op = true;
      investigate_anom_ord = &ord;
      investigate_anom_id = ord.anomaly_id;

      const auto* an = find_ptr(state_.anomalies, investigate_anom_id);
      if (!an || an->system_id != ship.system_id || an->resolved) { q.erase(q.begin()); continue; }

      // Placeholder gating: require some sensor capability to perform an investigation.
      const auto* d = find_design(ship.design_id);
      const double sensor_range = d ? std::max(0.0, d->sensor_range_mkm) : 0.0;
      if (sensor_range <= 1e-9) { q.erase(q.begin()); continue; }

      target = an->position_mkm;
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
      if (!tgt || tgt->system_id != ship.system_id || tgt->faction_id != ship.faction_id || tgt->id == ship.id) {
        q.erase(q.begin());
        continue;
      }
      target = tgt->position_mkm;
    } else if (std::holds_alternative<TransferFuelToShip>(q.front())) {
      auto& ord = std::get<TransferFuelToShip>(q.front());
      is_fuel_transfer_op = true;
      fuel_transfer_ord = &ord;
      fuel_target_ship_id = ord.target_ship_id;
      fuel_tons = ord.tons;

      const auto* tgt = find_ptr(state_.ships, fuel_target_ship_id);
      if (!tgt || tgt->system_id != ship.system_id || tgt->faction_id != ship.faction_id || tgt->id == ship.id) {
        q.erase(q.begin());
        continue;
      }
      const auto* src_d = find_design(ship.design_id);
      const auto* tgt_d = find_design(tgt->design_id);
      if (!src_d || !tgt_d || src_d->fuel_capacity_tons <= 0.0 || tgt_d->fuel_capacity_tons <= 0.0) {
        q.erase(q.begin());
        continue;
      }
      target = tgt->position_mkm;
    } else if (std::holds_alternative<TransferTroopsToShip>(q.front())) {
      auto& ord = std::get<TransferTroopsToShip>(q.front());
      is_troop_transfer_op = true;
      troop_transfer_ord = &ord;
      troop_target_ship_id = ord.target_ship_id;
      troop_transfer_strength = ord.strength;

      const auto* tgt = find_ptr(state_.ships, troop_target_ship_id);
      if (!tgt || tgt->system_id != ship.system_id || tgt->faction_id != ship.faction_id || tgt->id == ship.id) {
        q.erase(q.begin());
        continue;
      }
      const auto* src_d = find_design(ship.design_id);
      const auto* tgt_d = find_design(tgt->design_id);
      if (!src_d || !tgt_d || src_d->troop_capacity <= 0.0 || tgt_d->troop_capacity <= 0.0) {
        q.erase(q.begin());
        continue;
      }
      target = tgt->position_mkm;
    } else if (std::holds_alternative<TransferColonistsToShip>(q.front())) {
      auto& ord = std::get<TransferColonistsToShip>(q.front());
      is_colonist_transfer_op = true;
      colonist_transfer_ord = &ord;
      colonist_target_ship_id = ord.target_ship_id;
      colonist_transfer_millions = ord.millions;

      const auto* tgt = find_ptr(state_.ships, colonist_target_ship_id);
      if (!tgt || tgt->system_id != ship.system_id || tgt->faction_id != ship.faction_id || tgt->id == ship.id) {
        q.erase(q.begin());
        continue;
      }
      const auto* src_d = find_design(ship.design_id);
      const auto* tgt_d = find_design(tgt->design_id);
      if (!src_d || !tgt_d || src_d->colony_capacity_millions <= 0.0 || tgt_d->colony_capacity_millions <= 0.0) {
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
                              std::holds_alternative<AttackShip>(q.front()) ||
                              (std::holds_alternative<EscortShip>(q.front()) && !escort_is_jump_leg) ||
                              std::holds_alternative<BombardColony>(q.front());
      if (can_offset) {
        if (auto itoff = formation_offset_mkm.find(ship_id); itoff != formation_offset_mkm.end()) {
          target = target + itoff->second;
        }
      }
    }

    Vec2 delta = target - ship.position_mkm;
    double dist = delta.length();

    const bool is_attack = std::holds_alternative<AttackShip>(q.front());
    const bool is_escort = is_escort_op;
    const bool is_jump = std::holds_alternative<TravelViaJump>(q.front()) || escort_is_jump_leg;
    const bool is_move_body = std::holds_alternative<MoveToBody>(q.front());
    const bool is_colonize = std::holds_alternative<ColonizeBody>(q.front());
    const bool is_body = is_move_body || is_colonize;
    const bool is_orbit = std::holds_alternative<OrbitBody>(q.front());
    const bool is_bombard = std::holds_alternative<BombardColony>(q.front());
    const bool is_scrap = std::holds_alternative<ScrapShip>(q.front());

    // Optional kiting behaviour for AttackShip: if we are inside our desired
    // standoff range, back off instead of sitting at point-blank range.
    if (is_attack && attack_has_contact && ship.combat_doctrine.kite_if_too_close && desired_range > 1e-9) {
      const double dead = std::clamp(ship.combat_doctrine.kite_deadband_fraction, 0.0, 0.90);
      const double threshold = desired_range * (1.0 - dead);
      if (dist + 1e-9 < threshold) {
        Vec2 away = (dist > 1e-9) ? (ship.position_mkm - target).normalized() : Vec2{1.0, 0.0};
        const double need = std::max(0.0, desired_range - dist);
        if (need > 1e-9 && (std::abs(away.x) > 1e-12 || std::abs(away.y) > 1e-12)) {
          target = ship.position_mkm + away * need;
          // Move to the backoff point exactly; don't treat it as an "approach to desired_range".
          desired_range = 0.0;
          delta = target - ship.position_mkm;
          dist = delta.length();
        }
      }
    }

    // Fleet jump coordination: if multiple ships in the same fleet are trying to
    // transit the same jump point in the same system, we can optionally hold the
    // transit until all of them have arrived.
    bool is_coordinated_jump_group = false;
    bool allow_jump_transit = true;
    if (is_jump && cfg_.fleet_coordinated_jumps && fleet_id != kInvalidId && !jump_group_state.empty()) {
      const Id jump_id = escort_is_jump_leg ? escort_jump_id : std::get<TravelViaJump>(q.front()).jump_point_id;
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

      // If the destination is a ship, we may apply special handling (e.g.
      // munitions -> missile magazine reload).
      Ship* dest_ship = nullptr;

      Colony* cargo_colony = nullptr;

      if (cargo_mode == 0) { // Load from colony
        cargo_colony = find_ptr(state_.colonies, cargo_colony_id);
        if (!cargo_colony) return 0.0;
        source_minerals = &cargo_colony->minerals;
        dest_minerals = &ship.cargo;
        dest_ship = &ship;
        
        const auto* d = find_design(ship.design_id);
        const double cap = d ? d->cargo_tons : 0.0;
        dest_capacity_free = std::max(0.0, cap - cargo_used_tons(ship));

        // Munitions can be loaded into missile magazines even when the design
        // has no generic cargo capacity.
        if (cargo_mineral == kMunitionsKey && d && d->missile_ammo_capacity > 0) {
          dest_capacity_free = munitions_magazine_free_tons(ship);
        }
      } else if (cargo_mode == 1) { // Unload to colony
        cargo_colony = find_ptr(state_.colonies, cargo_colony_id);
        if (!cargo_colony) return 0.0;
        source_minerals = &ship.cargo;
        dest_minerals = &cargo_colony->minerals;
        // Colony has infinite capacity
      } else if (cargo_mode == 2) { // Transfer to ship
        auto* tgt = find_ptr(state_.ships, cargo_target_ship_id);
        if (!tgt) return 0.0;
        source_minerals = &ship.cargo;
        dest_minerals = &tgt->cargo;
        dest_ship = tgt;
        
        const auto* d = find_design(tgt->design_id);
        const double cap = d ? d->cargo_tons : 0.0;
        dest_capacity_free = std::max(0.0, cap - cargo_used_tons(*tgt));

        // Allow transferring Munitions into missile magazines even if the
        // target ship has zero generic cargo capacity.
        if (cargo_mineral == kMunitionsKey && d && d->missile_ammo_capacity > 0) {
          dest_capacity_free = munitions_magazine_free_tons(*tgt);
        }
      }

      if (!source_minerals || !dest_minerals) return 0.0;
      if (dest_capacity_free <= 1e-9) return 0.0;

      double moved_total = 0.0;
      double remaining_request = (cargo_tons > 0.0) ? cargo_tons : 1e300;
      remaining_request = std::min(remaining_request, dest_capacity_free);

      // Throughput limit per tick (prevents instant transfers at 1h ticks).
      if (dt_days > 0.0) {
        double cap_for_rate = 0.0;
        if (cargo_mode == 2) {
          const auto* src_d = find_design(ship.design_id);
          const auto* tgt = find_ptr(state_.ships, cargo_target_ship_id);
          const auto* tgt_d = tgt ? find_design(tgt->design_id) : nullptr;
          const double src_cap = src_d ? std::max(0.0, src_d->cargo_tons) : 0.0;
          const double tgt_cap = tgt_d ? std::max(0.0, tgt_d->cargo_tons) : 0.0;
          cap_for_rate = std::min(src_cap, tgt_cap);
        } else {
          const auto* src_d = find_design(ship.design_id);
          cap_for_rate = src_d ? std::max(0.0, src_d->cargo_tons) : 0.0;
        }

        const double per_ton = std::max(0.0, cfg_.cargo_transfer_tons_per_day_per_cargo_ton);
        const double min_rate = std::max(0.0, cfg_.cargo_transfer_tons_per_day_min);
        const double rate_per_day = std::max(min_rate, cap_for_rate * per_ton);
        remaining_request = std::min(remaining_request, rate_per_day * dt_days);
      }


      auto transfer_one = [&](const std::string& min_type, double amount_limit) {
        if (amount_limit <= 1e-9) return 0.0;
        auto it_src = source_minerals->find(min_type);
        const double have_raw = (it_src != source_minerals->end()) ? std::max(0.0, it_src->second) : 0.0;
        double have = have_raw;

        // Civilian trade convoy export safeguards (Merchant Guild loading from colonies).
        if (cargo_mode == 0 && allow_civilian_trade_cargo_ops && ship.faction_id == merchant_faction_id && cargo_colony) {
          double reserve_floor = 0.0;

          if (auto it = cargo_colony->mineral_reserves.find(min_type); it != cargo_colony->mineral_reserves.end()) {
            reserve_floor = std::max(reserve_floor, std::max(0.0, it->second));
          }
          if (auto it = cargo_colony->mineral_targets.find(min_type); it != cargo_colony->mineral_targets.end()) {
            reserve_floor = std::max(reserve_floor, std::max(0.0, it->second));
          }

          reserve_floor = std::max(reserve_floor,
                                   logistics_reserve_tons(cargo_colony->faction_id, cargo_colony_id, min_type));

          if (min_type == "Fuel") {
            reserve_floor =
                std::max(reserve_floor, std::max(0.0, cfg_.civilian_trade_convoy_export_min_fuel_reserve_tons));
          } else if (min_type == "Munitions") {
            reserve_floor =
                std::max(reserve_floor, std::max(0.0, cfg_.civilian_trade_convoy_export_min_munitions_reserve_tons));
          }

          const double mult = std::max(0.0, cfg_.civilian_trade_convoy_export_reserve_multiplier);
          reserve_floor *= mult;

          have = std::max(0.0, have_raw - reserve_floor);
        }

        const double take = std::min(have, amount_limit);
        if (take > 1e-9) {
          (*dest_minerals)[min_type] += take;
          if (it_src != source_minerals->end()) {
            it_src->second = std::max(0.0, it_src->second - take);
            if (it_src->second <= 1e-9) source_minerals->erase(it_src);
          }
          moved_total += take;

          // If we just transferred Munitions into a finite-ammo missile ship,
          // immediately reload missile ammo from the received munitions.
          if (dest_ship && min_type == kMunitionsKey) {
            reload_missile_ammo_from_munitions(*dest_ship);
          }
        }
        return take;
      };

      auto record_trade_activity = [&]() {
        // Record only real colony transfers performed by Merchant Guild civilian convoys.
        if (!(moved_total > 1e-9)) return;
        if (!(cargo_mode == 0 || cargo_mode == 1)) return;
        if (!allow_civilian_trade_cargo_ops) return;
        if (merchant_faction_id == kInvalidId || ship.faction_id != merchant_faction_id) return;
        auto it_sys = state_.systems.find(ship.system_id);
        if (it_sys == state_.systems.end()) return;
        if (!std::isfinite(it_sys->second.civilian_trade_activity_score) || it_sys->second.civilian_trade_activity_score < 0.0) {
          it_sys->second.civilian_trade_activity_score = 0.0;
        }
        it_sys->second.civilian_trade_activity_score += moved_total;

        // The prosperity cache is day-scoped; invalidate so this within-day
        // update becomes visible without waiting for midnight.
        invalidate_trade_prosperity_cache();
      };

      if (!cargo_mineral.empty()) {
        transfer_one(cargo_mineral, remaining_request);
        record_trade_activity();
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
      record_trade_activity();
      return moved_total;
    };

    auto cargo_order_complete = [&](double moved_this_tick) {
      if (cargo_tons <= 0.0) {
        // "As much as possible": keep the order until we can't move anything
        // (blocked by full cargo holds / empty source / etc.).
        return moved_this_tick <= 1e-9;
      }

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

    // Wreck salvage moves minerals from a wreck into this ship's cargo holds.
    auto do_wreck_salvage = [&]() -> double {
      auto* w = find_ptr(state_.wrecks, salvage_wreck_id);
      if (!w) return 0.0;
      if (w->system_id != ship.system_id) return 0.0;

      const auto* d = find_design(ship.design_id);
      const double cap = d ? d->cargo_tons : 0.0;
      const double free = std::max(0.0, cap - cargo_used_tons(ship));
      if (free <= 1e-9) return 0.0;

      double remaining_request = (salvage_tons > 0.0) ? salvage_tons : 1e300;
      remaining_request = std::min(remaining_request, free);

      // Salvage throughput limit (tons/day).
      if (dt_days > 0.0) {
        const double per_ton = std::max(0.0, cfg_.salvage_tons_per_day_per_cargo_ton);
        const double min_rate = std::max(0.0, cfg_.salvage_tons_per_day_min);
        const double rate_per_day = std::max(min_rate, cap * per_ton);
        remaining_request = std::min(remaining_request, rate_per_day * dt_days);
      }

      if (remaining_request <= 1e-9) return 0.0;

      double moved_total = 0.0;
      double salvage_rp_gain = 0.0;
      double reverse_engineering_points = 0.0;

      const bool salvage_research_enabled = cfg_.enable_salvage_research && cfg_.salvage_research_rp_multiplier > 0.0;
      const bool reverse_engineering_enabled = cfg_.enable_reverse_engineering && cfg_.reverse_engineering_points_per_salvaged_ton > 0.0;
      const bool can_reverse_engineer_this_wreck =
          reverse_engineering_enabled &&
          w->kind == WreckKind::Ship &&
          !w->source_design_id.empty() &&
          w->source_faction_id != ship.faction_id;

      auto apply_reverse_engineering = [&](const Wreck& wreck, double points) {
        if (!cfg_.enable_reverse_engineering) return;
        if (points <= 1e-9) return;
        if (wreck.kind != WreckKind::Ship) return;
        if (wreck.source_design_id.empty()) return;
        if (wreck.source_faction_id == ship.faction_id) return;

        auto* fac = find_ptr(state_.factions, ship.faction_id);
        if (!fac) return;

        const ShipDesign* src_design = find_design(wreck.source_design_id);
        if (!src_design) return;

        std::vector<std::string> candidates;
        candidates.reserve(src_design->components.size());
        for (const auto& cid : src_design->components) {
          if (cid.empty()) continue;
          if (std::find(fac->unlocked_components.begin(), fac->unlocked_components.end(), cid) != fac->unlocked_components.end()) continue;
          candidates.push_back(cid);
        }
        if (candidates.empty()) return;
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

        const double per = points / static_cast<double>(candidates.size());
        for (const auto& cid : candidates) {
          if (per <= 0.0) break;
          fac->reverse_engineering_progress[cid] += per;
        }

        // Unlock any components that crossed the threshold.
        int unlock_count = 0;
        std::vector<std::string> unlocked;
        for (const auto& cid : candidates) {
          const auto itp = fac->reverse_engineering_progress.find(cid);
          if (itp == fac->reverse_engineering_progress.end()) continue;
          const double req = reverse_engineering_points_required_for_component(cid);
          if (req <= 0.0) continue;
          if (itp->second + 1e-9 < req) continue;

          // Unlock and drop progress.
          fac->unlocked_components.push_back(cid);
          fac->reverse_engineering_progress.erase(itp);
          unlocked.push_back(cid);
          ++unlock_count;

          const int cap = cfg_.reverse_engineering_unlock_cap_per_tick;
          if (cap > 0 && unlock_count >= cap) break;
        }

        if (!unlocked.empty()) {
          std::sort(fac->unlocked_components.begin(), fac->unlocked_components.end());
          fac->unlocked_components.erase(std::unique(fac->unlocked_components.begin(), fac->unlocked_components.end()),
                                         fac->unlocked_components.end());

          std::ostringstream ss;
          ss << "Reverse engineering complete: ";
          for (std::size_t i = 0; i < unlocked.size(); ++i) {
            const std::string& cid = unlocked[i];
            const auto itc = content_.components.find(cid);
            const std::string cname = (itc != content_.components.end() && !itc->second.name.empty()) ? itc->second.name : cid;
            if (i) ss << ", ";
            ss << cname;
          }
          if (!wreck.name.empty()) ss << " (from " << wreck.name << ")";

          EventContext ctx;
          ctx.system_id = ship.system_id;
          ctx.ship_id = ship_id;
          ctx.faction_id = ship.faction_id;
          push_event(EventLevel::Info, EventCategory::Research, ss.str(), ctx);
        }
      };

      auto transfer_one = [&](const std::string& min_type, double amount_limit) {
        if (amount_limit <= 1e-9) return 0.0;
        auto it_src = w->minerals.find(min_type);
        const double have = (it_src != w->minerals.end()) ? std::max(0.0, it_src->second) : 0.0;
        const double take = std::min(have, amount_limit);
        if (take > 1e-9) {
          ship.cargo[min_type] += take;
          it_src->second = std::max(0.0, it_src->second - take);
          if (it_src->second <= 1e-9) w->minerals.erase(it_src);
          moved_total += take;

          if (salvage_research_enabled) {
            const auto it_r = content_.resources.find(min_type);
            if (it_r != content_.resources.end() && it_r->second.salvage_research_rp_per_ton > 0.0) {
              salvage_rp_gain += take * it_r->second.salvage_research_rp_per_ton;
            }
          }

          if (can_reverse_engineer_this_wreck) {
            reverse_engineering_points += take * cfg_.reverse_engineering_points_per_salvaged_ton;
          }
        }
        return take;
      };

      if (!salvage_mineral.empty()) {
        transfer_one(salvage_mineral, remaining_request);
      } else {
        std::vector<std::string> keys;
        keys.reserve(w->minerals.size());
        for (const auto& [k, v] : w->minerals) {
          if (v > 1e-9) keys.push_back(k);
        }
        std::sort(keys.begin(), keys.end());
        for (const auto& k : keys) {
          if (remaining_request <= 1e-9) break;
          const double moved = transfer_one(k, remaining_request);
          remaining_request -= moved;
        }
      }

      // Apply salvage research + reverse engineering rewards.
      if (salvage_research_enabled && salvage_rp_gain > 1e-9) {
        salvage_rp_gain *= std::max(0.0, cfg_.salvage_research_rp_multiplier);
        if (auto* fac = find_ptr(state_.factions, ship.faction_id)) {
          fac->research_points += salvage_rp_gain;
        }
      }

      if (can_reverse_engineer_this_wreck && reverse_engineering_points > 1e-9) {
        apply_reverse_engineering(*w, reverse_engineering_points);
      }

      // If emptied, remove the wreck from the game state and emit a completion event.
      if (w->minerals.empty()) {
        const Wreck w_copy = *w;
        state_.wrecks.erase(salvage_wreck_id);

        EventContext ctx;
        ctx.system_id = ship.system_id;
        ctx.ship_id = ship_id;
        ctx.faction_id = ship.faction_id;
        const std::string nm = w_copy.name.empty() ? std::string("(unknown wreck)") : w_copy.name;
        const std::string msg = "Salvage complete: " + nm;
        push_event(EventLevel::Info, EventCategory::Exploration, msg, ctx);
      }

      return moved_total;
    };

    auto salvage_order_complete = [&](double moved_this_tick) {
      if (salvage_tons <= 0.0) {
        // "As much as possible": keep salvaging until blocked or the wreck is gone.
        if (!find_ptr(state_.wrecks, salvage_wreck_id)) return true;
        return moved_this_tick <= 1e-9;
      }

      if (salvage_ord) {
        salvage_ord->tons = std::max(0.0, salvage_ord->tons - moved_this_tick);
        salvage_tons = salvage_ord->tons;
      }

      if (salvage_tons <= 1e-9) return true;
      if (moved_this_tick <= 1e-9) return true; // blocked
      return false;
    };

    // Mobile mining moves minerals from a body's deposits into this ship's cargo holds.
    auto do_body_mining = [&]() -> double {
      if (mine_body_id == kInvalidId) return 0.0;
      auto* body = find_ptr(state_.bodies, mine_body_id);
      if (!body) return 0.0;
      if (body->system_id != ship.system_id) return 0.0;

      const auto* d = find_design(ship.design_id);
      if (!d) return 0.0;

      // Base mining rate (tons/day) from design; apply faction mining multiplier if present.
      double rate_per_day = std::max(0.0, d->mining_tons_per_day);
      if (const auto* fac = find_ptr(state_.factions, ship.faction_id)) {
        const auto mult = sim_internal::compute_faction_economy_multipliers(content_, *fac);
        rate_per_day *= std::max(0.0, mult.mining);
      }

      if (rate_per_day <= 1e-12) return 0.0;

      const double cap = std::max(0.0, d->cargo_tons);
      const double free = std::max(0.0, cap - cargo_used_tons(ship));
      if (free <= 1e-9) return 0.0;

      double remaining = std::min(free, rate_per_day * dt_days);
      if (remaining <= 1e-9) return 0.0;

      auto mine_one = [&](const std::string& mineral, double want) -> double {
        if (want <= 1e-9) return 0.0;

        auto it = body->mineral_deposits.find(mineral);
        if (it == body->mineral_deposits.end()) {
          // Deposit semantics:
          // - If the deposit map is empty, treat missing keys as unlimited (legacy).
          // - Otherwise, missing keys mean the mineral isn't present on this body.
          if (body->mineral_deposits.empty()) {
            ship.cargo[mineral] += want;
            return want;
          }
          return 0.0;
        }

        const double have = std::max(0.0, it->second);
        if (have <= 1e-9) return 0.0;

        const double take = std::min(have, want);
        if (take <= 1e-9) return 0.0;

        ship.cargo[mineral] += take;

        const double before = it->second;
        it->second = std::max(0.0, it->second - take);

        // Depletion event (only on transition to depleted).
        if (before > 1e-9 && it->second <= 1e-9) {
          EventContext ctx;
          ctx.system_id = body->system_id;
          ctx.ship_id = ship_id;
          ctx.faction_id = ship.faction_id;
          const std::string msg = "Mineral deposit depleted on " + body->name + ": " + mineral + " (mobile mining)";
          push_event(EventLevel::Warn, EventCategory::Construction, msg, ctx);
        }

        return take;
      };

      double mined_total = 0.0;

      if (!mine_mineral.empty()) {
        const double mined = mine_one(mine_mineral, remaining);
        mined_total += mined;
        return mined_total;
      }

      // Mine all modeled minerals on the body (deterministic order).
      if (body->mineral_deposits.empty()) return 0.0;
      for (const auto& k : sorted_keys(body->mineral_deposits)) {
        if (remaining <= 1e-9) break;
        const double mined = mine_one(k, remaining);
        remaining -= mined;
        mined_total += mined;
      }
      return mined_total;
    };

    auto mining_order_complete = [&](double mined_this_tick) {
      (void)mined_this_tick;
      if (!mine_ord) return true;

      const auto* d = find_design(ship.design_id);
      const double cap = d ? std::max(0.0, d->cargo_tons) : 0.0;
      const double free = std::max(0.0, cap - cargo_used_tons(ship));

      if (mine_stop_when_full && free <= 1e-9) return true;

      auto* body = find_ptr(state_.bodies, mine_body_id);
      if (!body) return true;

      // If the player asked to "mine all", but this body has no modeled deposits,
      // the order can't make progress (we don't have a mineral list).
      if (mine_mineral.empty() && body->mineral_deposits.empty()) return true;

      if (!mine_mineral.empty()) {
        auto it = body->mineral_deposits.find(mine_mineral);
        if (it != body->mineral_deposits.end()) {
          // Modeled deposit: stop once depleted.
          return it->second <= 1e-9;
        }
        if (body->mineral_deposits.empty()) {
          // Legacy bodies without modeled deposits: missing key => unlimited.
          return false;
        }
        // Mineral not present on this body: nothing to mine.
        return true;
      }

      // Mine all modeled minerals: stop when all deposits are depleted.
      bool any_left = false;
      for (const auto& [_, v] : body->mineral_deposits) {
        if (v > 1e-9) { any_left = true; break; }
      }
      if (!any_left) return true;

      // Otherwise keep mining (even if we mined 0 this tick, e.g., due to 0 free cargo).
      return false;
    };

    // Fuel transfer is handled similarly to cargo transfer, but operates on
    // ships' fuel tanks rather than cargo holds.
    auto do_fuel_transfer = [&]() -> double {
      auto* tgt = find_ptr(state_.ships, fuel_target_ship_id);
      if (!tgt) return 0.0;
      if (tgt->faction_id != ship.faction_id) return 0.0;
      if (tgt->system_id != ship.system_id) return 0.0;

      const auto* src_d = find_design(ship.design_id);
      const auto* tgt_d = find_design(tgt->design_id);
      const double src_cap = src_d ? std::max(0.0, src_d->fuel_capacity_tons) : 0.0;
      const double tgt_cap = tgt_d ? std::max(0.0, tgt_d->fuel_capacity_tons) : 0.0;
      if (src_cap <= 1e-9 || tgt_cap <= 1e-9) return 0.0;

      // Clamp for safety: older saves / refits could momentarily violate caps.
      ship.fuel_tons = std::max(0.0, std::min(ship.fuel_tons, src_cap));
      tgt->fuel_tons = std::max(0.0, std::min(tgt->fuel_tons, tgt_cap));

      const double free = std::max(0.0, tgt_cap - tgt->fuel_tons);
      if (free <= 1e-9) return 0.0;

      double remaining_request = (fuel_tons > 0.0) ? fuel_tons : 1e300;
      remaining_request = std::min(remaining_request, free);

      // Throughput limit per tick (prevents instant ship-to-ship refueling at 1h ticks).
      if (dt_days > 0.0) {
        const double cap_for_rate = std::min(src_cap, tgt_cap);
        const double per_ton = std::max(0.0, cfg_.fuel_transfer_tons_per_day_per_fuel_ton);
        const double min_rate = std::max(0.0, cfg_.fuel_transfer_tons_per_day_min);
        const double rate_per_day = std::max(min_rate, cap_for_rate * per_ton);
        remaining_request = std::min(remaining_request, rate_per_day * dt_days);
      }

      if (remaining_request <= 1e-9) return 0.0;


      const double have = std::max(0.0, ship.fuel_tons);
      const double give = std::min(have, remaining_request);
      if (give <= 1e-9) return 0.0;

      ship.fuel_tons -= give;
      tgt->fuel_tons += give;
      return give;
    };

    auto fuel_order_complete = [&](double moved_this_tick) {
      if (fuel_tons <= 0.0) {
        // "As much as possible": keep the order until we can't move anything
        // (source empty / target full / etc.).
        return moved_this_tick <= 1e-9;
      }

      if (fuel_transfer_ord) {
        fuel_transfer_ord->tons = std::max(0.0, fuel_transfer_ord->tons - moved_this_tick);
        fuel_tons = fuel_transfer_ord->tons;
      }

      if (fuel_tons <= 1e-9) return true;

      // If we couldn't move anything this tick, we are done or blocked.
      if (moved_this_tick <= 1e-9) return true;
      return false;
    };

    // Troop transfer is handled similarly to fuel transfer, but operates on
    // embarked troops and troop bay capacities.
    auto do_troop_transfer = [&]() -> double {
      auto* tgt = find_ptr(state_.ships, troop_target_ship_id);
      if (!tgt) return 0.0;
      if (tgt->faction_id != ship.faction_id) return 0.0;
      if (tgt->system_id != ship.system_id) return 0.0;

      const auto* src_d = find_design(ship.design_id);
      const auto* tgt_d = find_design(tgt->design_id);
      const double src_cap = src_d ? std::max(0.0, src_d->troop_capacity) : 0.0;
      const double tgt_cap = tgt_d ? std::max(0.0, tgt_d->troop_capacity) : 0.0;
      if (src_cap <= 1e-9 || tgt_cap <= 1e-9) return 0.0;

      // Clamp for safety: older saves / refits could momentarily violate caps.
      ship.troops = std::max(0.0, std::min(ship.troops, src_cap));
      tgt->troops = std::max(0.0, std::min(tgt->troops, tgt_cap));

      const double free = std::max(0.0, tgt_cap - tgt->troops);
      if (free <= 1e-9) return 0.0;

      double remaining_request = (troop_transfer_strength > 0.0) ? troop_transfer_strength : 1e300;
      remaining_request = std::min(remaining_request, free);

      // Throughput limit per tick (prevents instant ship-to-ship troop transfers at 1h ticks).
      if (dt_days > 0.0) {
        const double cap_for_rate = std::min(src_cap, tgt_cap);
        const double per_cap = std::max(0.0, cfg_.troop_transfer_strength_per_day_per_troop_cap);
        const double min_rate = std::max(0.0, cfg_.troop_transfer_strength_per_day_min);
        const double rate_per_day = std::max(min_rate, cap_for_rate * per_cap);
        remaining_request = std::min(remaining_request, rate_per_day * dt_days);
      }

      if (remaining_request <= 1e-9) return 0.0;


      const double have = std::max(0.0, ship.troops);
      const double give = std::min(have, remaining_request);
      if (give <= 1e-9) return 0.0;

      ship.troops -= give;
      tgt->troops += give;
      return give;
    };

    auto troop_transfer_order_complete = [&](double moved_this_tick) {
      if (troop_transfer_strength <= 0.0) {
        // "As much as possible": keep the order until we can't move anything
        // (source empty / target full / etc.).
        return moved_this_tick <= 1e-9;
      }

      if (troop_transfer_ord) {
        troop_transfer_ord->strength = std::max(0.0, troop_transfer_ord->strength - moved_this_tick);
        troop_transfer_strength = troop_transfer_ord->strength;
      }

      if (troop_transfer_strength <= 1e-9) return true;
      if (moved_this_tick <= 1e-9) return true; // blocked
      return false;
    };

    // Colonist transfer mirrors troop transfer, but operates on embarked colonists
    // and colony module capacities.
    auto do_colonist_transfer = [&]() -> double {
      auto* tgt = find_ptr(state_.ships, colonist_target_ship_id);
      if (!tgt) return 0.0;
      if (tgt->faction_id != ship.faction_id) return 0.0;
      if (tgt->system_id != ship.system_id) return 0.0;

      const auto* src_d = find_design(ship.design_id);
      const auto* tgt_d = find_design(tgt->design_id);
      const double src_cap = src_d ? std::max(0.0, src_d->colony_capacity_millions) : 0.0;
      const double tgt_cap = tgt_d ? std::max(0.0, tgt_d->colony_capacity_millions) : 0.0;
      if (src_cap <= 1e-9 || tgt_cap <= 1e-9) return 0.0;

      // Clamp for safety: older saves / refits could momentarily violate caps.
      ship.colonists_millions = std::max(0.0, std::min(ship.colonists_millions, src_cap));
      tgt->colonists_millions = std::max(0.0, std::min(tgt->colonists_millions, tgt_cap));

      const double free = std::max(0.0, tgt_cap - tgt->colonists_millions);
      if (free <= 1e-9) return 0.0;

      double remaining_request = (colonist_transfer_millions > 0.0) ? colonist_transfer_millions : 1e300;
      remaining_request = std::min(remaining_request, free);

      // Throughput limit per tick (prevents instant ship-to-ship population transfers at 1h ticks).
      if (dt_days > 0.0) {
        const double cap_for_rate = std::min(src_cap, tgt_cap);
        const double per_cap = std::max(0.0, cfg_.colonist_transfer_millions_per_day_per_colony_cap);
        const double min_rate = std::max(0.0, cfg_.colonist_transfer_millions_per_day_min);
        const double rate_per_day = std::max(min_rate, cap_for_rate * per_cap);
        remaining_request = std::min(remaining_request, rate_per_day * dt_days);
      }

      if (remaining_request <= 1e-9) return 0.0;

      const double have = std::max(0.0, ship.colonists_millions);
      const double give = std::min(have, remaining_request);
      if (give <= 1e-9) return 0.0;

      ship.colonists_millions -= give;
      tgt->colonists_millions += give;
      return give;
    };

    auto colonist_transfer_order_complete = [&](double moved_this_tick) {
      if (colonist_transfer_millions <= 0.0) {
        // "As much as possible": keep the order until we can't move anything
        // (source empty / target full / etc.).
        return moved_this_tick <= 1e-9;
      }

      if (colonist_transfer_ord) {
        colonist_transfer_ord->millions = std::max(0.0, colonist_transfer_ord->millions - moved_this_tick);
        colonist_transfer_millions = colonist_transfer_ord->millions;
      }

      if (colonist_transfer_millions <= 1e-9) return true;
      if (moved_this_tick <= 1e-9) return true; // blocked
      return false;
    };

    auto process_salvage_loop_docked = [&]() {
      // Note: We intentionally do not allow the generic cargo/salvage completion logic
      // to erase this order. Instead, we transition between salvage <-> unload modes.
      auto ord = std::get<SalvageWreckLoop>(q.front());
      if (ord.mode != 0 && ord.mode != 1) ord.mode = 0;

      const auto* d = find_design(ship.design_id);
      const double cargo_cap = d ? std::max(0.0, d->cargo_tons) : 0.0;

      auto is_valid_dropoff = [&](Id colony_id) -> bool {
        if (colony_id == kInvalidId) return false;
        const auto* col = find_ptr(state_.colonies, colony_id);
        if (!col) return false;
        if (col->faction_id != ship.faction_id) return false;
        const auto* body = find_ptr(state_.bodies, col->body_id);
        if (!body) return false;
        if (body->system_id == kInvalidId) return false;
        return true;
      };

      auto pick_best_dropoff = [&]() -> Id {
        if (ship.system_id == kInvalidId) return kInvalidId;
        Id best = kInvalidId;
        double best_eta = std::numeric_limits<double>::infinity();
        for (const auto& [cid, col] : state_.colonies) {
          if (col.faction_id != ship.faction_id) continue;
          const auto* body = find_ptr(state_.bodies, col.body_id);
          if (!body || body->system_id == kInvalidId) continue;
          const auto plan = plan_jump_route_cached(ship.system_id, ship.position_mkm, ship.faction_id, ship.speed_km_s,
                                                   body->system_id, ord.restrict_to_discovered, body->position_mkm);
          if (!plan) continue;
          const double eta = std::isfinite(plan->total_eta_days) ? plan->total_eta_days : plan->total_distance_mkm;
          if (eta < best_eta) {
            best_eta = eta;
            best = cid;
          }
        }
        return best;
      };

      auto queue_route_then_update_order = [&](Id target_system_id, const Vec2& goal_pos_mkm, int mode,
                                              Id dropoff_colony_id) {
        ord.mode = mode;
        ord.dropoff_colony_id = dropoff_colony_id;

        if (target_system_id == ship.system_id) {
          std::get<SalvageWreckLoop>(q.front()) = ord;
          return;
        }
        if (target_system_id == kInvalidId || ship.system_id == kInvalidId) {
          q.erase(q.begin());
          return;
        }

        const auto plan = plan_jump_route_cached(ship.system_id, ship.position_mkm, ship.faction_id, ship.speed_km_s,
                                                 target_system_id, ord.restrict_to_discovered, goal_pos_mkm);
        if (!plan || (plan->jump_ids.empty() && target_system_id != ship.system_id)) {
          q.erase(q.begin());
          return;
        }

        std::vector<Order> legs;
        legs.reserve(plan->jump_ids.size());
        for (Id jid : plan->jump_ids) legs.emplace_back(TravelViaJump{jid});

        q.erase(q.begin());
        q.insert(q.begin(), legs.begin(), legs.end());
        q.insert(q.begin() + legs.size(), ord);
      };

      if (ord.mode == 0) {
        // Salvage mode.
        do_wreck_salvage();

        const double used = cargo_used_tons(ship);
        const double free = std::max(0.0, cargo_cap - used);
        const bool cargo_has = used > 1e-9;

        const auto* w = find_ptr(state_.wrecks, ord.wreck_id);
        if (!w) {
          // Wreck depleted (or missing). Deliver the final load if any, else finish.
          if (!cargo_has) {
            q.erase(q.begin());
            return;
          }

          Id drop = is_valid_dropoff(ord.dropoff_colony_id) ? ord.dropoff_colony_id : pick_best_dropoff();
          if (!is_valid_dropoff(drop)) {
            q.erase(q.begin());
            return;
          }
          const auto* col = find_ptr(state_.colonies, drop);
          const auto* body = col ? find_ptr(state_.bodies, col->body_id) : nullptr;
          if (!body) {
            q.erase(q.begin());
            return;
          }
          queue_route_then_update_order(body->system_id, body->position_mkm, /*mode=*/1, drop);
          return;
        }

        // Still salvageable.
        if (cargo_has && free <= 1e-9) {
          // Cargo full; go unload.
          Id drop = is_valid_dropoff(ord.dropoff_colony_id) ? ord.dropoff_colony_id : pick_best_dropoff();
          if (!is_valid_dropoff(drop)) {
            q.erase(q.begin());
            return;
          }
          const auto* col = find_ptr(state_.colonies, drop);
          const auto* body = col ? find_ptr(state_.bodies, col->body_id) : nullptr;
          if (!body) {
            q.erase(q.begin());
            return;
          }
          queue_route_then_update_order(body->system_id, body->position_mkm, /*mode=*/1, drop);
          return;
        }

        // Keep salvaging.
        std::get<SalvageWreckLoop>(q.front()) = ord;
        return;
      }

      // Unload mode.
      const double moved = do_cargo_transfer();
      const double used = cargo_used_tons(ship);
      if (used <= 1e-9) {
        // Cargo empty; return to the wreck if it still exists.
        const auto* w = find_ptr(state_.wrecks, ord.wreck_id);
        if (!w) {
          q.erase(q.begin());
          return;
        }
        queue_route_then_update_order(w->system_id, w->position_mkm, /*mode=*/0, ord.dropoff_colony_id);
        return;
      }

      // If we can't move anything while unloading, avoid getting stuck forever.
      if (moved <= 1e-9) {
        q.erase(q.begin());
        return;
      }

      std::get<SalvageWreckLoop>(q.front()) = ord;
    };

    // --- Docking / Arrival Checks ---

    if (is_fuel_transfer_op && dist <= dock_range) {
      ship.position_mkm = target;
      const double moved = do_fuel_transfer();
      if (fuel_order_complete(moved)) q.erase(q.begin());
      continue;
    }

    if (is_troop_transfer_op && dist <= dock_range) {
      ship.position_mkm = target;
      const double moved = do_troop_transfer();
      if (troop_transfer_order_complete(moved)) q.erase(q.begin());
      continue;
    }

    if (is_colonist_transfer_op && dist <= dock_range) {
      ship.position_mkm = target;
      const double moved = do_colonist_transfer();
      if (colonist_transfer_order_complete(moved)) q.erase(q.begin());
      continue;
    }

    if (is_salvage_loop_op && dist <= dock_range) {
      ship.position_mkm = target;
      process_salvage_loop_docked();
      continue;
    }

    if (is_cargo_op && dist <= dock_range) {
      ship.position_mkm = target;
      const double moved = do_cargo_transfer();
      if (cargo_order_complete(moved)) q.erase(q.begin());
      continue;
    }

    if (is_salvage_op && dist <= dock_range) {
      ship.position_mkm = target;
      const double moved = do_wreck_salvage();
      if (salvage_order_complete(moved)) q.erase(q.begin());
      continue;
    }

    if (is_investigate_anomaly_op && dist <= dock_range) {
      ship.position_mkm = target;

      auto* anom = find_ptr(state_.anomalies, investigate_anom_id);
      if (!anom || anom->system_id != ship.system_id || anom->resolved) {
        q.erase(q.begin());
        continue;
      }

      // Ensure the investigating faction has intel on this anomaly.
      discover_anomaly_for_faction(ship.faction_id, anom->id, ship.id);

      if (investigate_anom_ord) {
        if (investigate_anom_ord->duration_days > 0) {
          investigate_anom_ord->progress_days = std::max(0.0, investigate_anom_ord->progress_days) + dt_days;
          while (investigate_anom_ord->duration_days > 0 && investigate_anom_ord->progress_days >= 1.0 - 1e-12) {
            investigate_anom_ord->duration_days -= 1;
            investigate_anom_ord->progress_days -= 1.0;
          }
        }

        if (investigate_anom_ord->duration_days <= 0) {
          // Resolve + award.
          anom->resolved = true;
          anom->resolved_by_faction_id = ship.faction_id;
          anom->resolved_day = state_.date.days_since_epoch();

          const double rp = std::max(0.0, anom->research_reward);

          // Direct component unlock (rare; typically from deep ruins/phenomena sites).
          bool unlocked_component = false;
          std::string unlocked_component_id;
          bool direct_unlock_configured = false;
          bool direct_unlock_redundant = false;

          // Anomaly schematic fragments (optional): exploration can contribute partial
          // reverse-engineering progress toward otherwise-locked components.
          struct SchematicFragment {
            std::string component_id;
            double points_added{0.0};
            double points_total{0.0};
            double points_required{0.0};
            bool unlocked{false};
          };

          std::vector<SchematicFragment> schematic_frags;
          procgen_obscure::ThemeDomain schematic_domain = procgen_obscure::ThemeDomain::Sensors;
          double schematic_points_total = 0.0;

          Faction* fac = find_ptr(state_.factions, ship.faction_id);
          if (fac) {
            if (rp > 1e-9) fac->research_points += rp;

            if (!anom->unlock_component_id.empty()) {
              // Only unlock known content components (prevents invalid saves).
              if (content_.components.find(anom->unlock_component_id) != content_.components.end()) {
                direct_unlock_configured = true;
                const bool already = std::find(fac->unlocked_components.begin(), fac->unlocked_components.end(),
                                               anom->unlock_component_id) != fac->unlocked_components.end();
                if (!already) {
                  fac->unlocked_components.push_back(anom->unlock_component_id);
                  unlocked_component = true;
                  unlocked_component_id = anom->unlock_component_id;
                } else {
                  direct_unlock_redundant = true;
                }
              }
            }

            // Apply schematic fragments (reverse-engineering points).
            if (cfg_.enable_reverse_engineering && cfg_.enable_anomaly_schematic_fragments) {
              const bool allow_stack = cfg_.anomaly_schematic_allow_with_direct_unlock || direct_unlock_redundant;
              if (!direct_unlock_configured || allow_stack) {
                // Compute points budget.
                double pts = std::max(0.0, cfg_.anomaly_schematic_points_base);
                pts += std::max(0.0, cfg_.anomaly_schematic_points_per_investigation_day) *
                       static_cast<double>(std::max(1, anom->investigation_days));
                pts += std::max(0.0, cfg_.anomaly_schematic_points_per_rp) * rp;

                // Kind multiplier.
                double km = 1.0;
                if (anom->kind == "ruins" || anom->kind == "artifact") km = cfg_.anomaly_schematic_ruins_multiplier;
                else if (anom->kind == "signal") km = cfg_.anomaly_schematic_signal_multiplier;
                else if (anom->kind == "distress") km = cfg_.anomaly_schematic_distress_multiplier;
                else if (anom->kind == "phenomenon") km = cfg_.anomaly_schematic_phenomenon_multiplier;

                // Risk bonus: hazardous sites are more likely to yield intact data cores.
                const double hz = std::clamp(anom->hazard_chance, 0.0, 1.0);
                const double risk_mult = 1.0 + 0.35 * hz;

                // Lead depth bonus: deeper chains skew slightly richer.
                const double depth_mult = 1.0 + 0.12 * static_cast<double>(std::max(0, anom->lead_depth));

                pts *= std::max(0.0, km) * risk_mult * depth_mult;
                if (!std::isfinite(pts) || pts <= 1e-9) pts = 0.0;

                // Pick 1..N not-yet-unlocked components biased by the anomaly theme domain.
                if (pts > 1e-9) {
                  schematic_points_total = pts;
                  schematic_domain = procgen_obscure::anomaly_theme_domain(*anom);

                  auto is_unlocked = [&](const std::string& cid) {
                    return std::find(fac->unlocked_components.begin(), fac->unlocked_components.end(), cid) !=
                           fac->unlocked_components.end();
                  };

                  auto build_candidates = [&](const std::vector<ComponentType>& types) {
                    std::vector<std::string> out;
                    out.reserve(content_.components.size());
                    for (const auto& [cid, def] : content_.components) {
                      if (cid.empty()) continue;
                      if (is_unlocked(cid)) continue;
                      if (!types.empty() && std::find(types.begin(), types.end(), def.type) == types.end()) continue;
                      out.push_back(cid);
                    }
                    std::sort(out.begin(), out.end());
                    out.erase(std::unique(out.begin(), out.end()), out.end());
                    return out;
                  };

                  std::vector<ComponentType> domain_types;
                  switch (schematic_domain) {
                    case procgen_obscure::ThemeDomain::Sensors:
                      domain_types = {ComponentType::Sensor};
                      break;
                    case procgen_obscure::ThemeDomain::Weapons:
                      domain_types = {ComponentType::Weapon, ComponentType::Armor, ComponentType::Shield};
                      break;
                    case procgen_obscure::ThemeDomain::Propulsion:
                      domain_types = {ComponentType::Engine, ComponentType::FuelTank};
                      break;
                    case procgen_obscure::ThemeDomain::Industry:
                      domain_types = {ComponentType::Mining, ComponentType::Cargo, ComponentType::ColonyModule};
                      break;
                    case procgen_obscure::ThemeDomain::Energy:
                      domain_types = {ComponentType::Reactor, ComponentType::Shield, ComponentType::Sensor};
                      break;
                    default:
                      break;
                  }

                  std::vector<ComponentType> kind_types;
                  if (anom->kind == "signal") {
                    kind_types = {ComponentType::Sensor, ComponentType::Reactor, ComponentType::Shield};
                  } else if (anom->kind == "phenomenon") {
                    kind_types = {ComponentType::Engine, ComponentType::Shield, ComponentType::Sensor, ComponentType::Reactor};
                  } else if (anom->kind == "ruins" || anom->kind == "artifact") {
                    kind_types = {ComponentType::Weapon, ComponentType::Armor, ComponentType::Shield, ComponentType::Reactor,
                                  ComponentType::Sensor};
                  } else if (anom->kind == "distress") {
                    kind_types = {ComponentType::Sensor, ComponentType::Engine, ComponentType::Reactor, ComponentType::Cargo};
                  }

                  std::vector<ComponentType> allowed;
                  if (!domain_types.empty() && !kind_types.empty()) {
                    for (ComponentType t : domain_types) {
                      if (std::find(kind_types.begin(), kind_types.end(), t) != kind_types.end()) allowed.push_back(t);
                    }
                  }
                  if (allowed.empty()) {
                    if (!domain_types.empty()) allowed = domain_types;
                    else allowed = kind_types;
                  }

                  std::vector<std::string> candidates = build_candidates(allowed);
                  if (candidates.empty()) candidates = build_candidates(domain_types);
                  if (candidates.empty()) candidates = build_candidates(kind_types);
                  if (candidates.empty()) candidates = build_candidates({});

                  const int want = std::clamp(cfg_.anomaly_schematic_components_per_anomaly, 1, 3);
                  const int n = std::min<int>(want, static_cast<int>(candidates.size()));

                  if (n > 0) {
                    const std::uint64_t seed = splitmix64(procgen_obscure::anomaly_seed(*anom) ^
                                                         (static_cast<std::uint64_t>(ship.faction_id) * 0x9e3779b97f4a7c15ULL) ^
                                                         (static_cast<std::uint64_t>(ship_id) * 0xbf58476d1ce4e5b9ULL) ^
                                                         0x534348454D415449ULL);  // "SCHEMATI"
                    HashRng rng(seed);

                    const double per = pts / static_cast<double>(n);
                    int unlock_count = 0;
                    const int unlock_cap = cfg_.reverse_engineering_unlock_cap_per_tick;

                    std::vector<std::string> unlocked_now;

                    for (int i = 0; i < n; ++i) {
                      if (candidates.empty()) break;
                      const int idx = rng.range_int(0, static_cast<int>(candidates.size()) - 1);
                      const std::string cid = candidates[static_cast<std::size_t>(idx)];
                      candidates.erase(candidates.begin() + idx);
                      if (cid.empty()) continue;
                      if (is_unlocked(cid)) continue;

                      double& cur = fac->reverse_engineering_progress[cid];
                      if (!std::isfinite(cur) || cur < 0.0) cur = 0.0;
                      cur += per;

                      const double req = reverse_engineering_points_required_for_component(cid);

                      SchematicFragment frag;
                      frag.component_id = cid;
                      frag.points_added = per;
                      frag.points_total = cur;
                      frag.points_required = req;

                      if (req > 0.0 && cur + 1e-9 >= req) {
                        fac->unlocked_components.push_back(cid);
                        fac->reverse_engineering_progress.erase(cid);
                        frag.unlocked = true;
                        unlocked_now.push_back(cid);
                        ++unlock_count;
                      }

                      schematic_frags.push_back(std::move(frag));

                      if (unlock_cap > 0 && unlock_count >= unlock_cap) break;
                    }

                    if (!unlocked_now.empty()) {
                      // De-dup and keep unlocked_components stable.
                      std::sort(fac->unlocked_components.begin(), fac->unlocked_components.end());
                      fac->unlocked_components.erase(std::unique(fac->unlocked_components.begin(), fac->unlocked_components.end()),
                                                     fac->unlocked_components.end());

                      std::sort(unlocked_now.begin(), unlocked_now.end());
                      unlocked_now.erase(std::unique(unlocked_now.begin(), unlocked_now.end()), unlocked_now.end());

                      std::ostringstream rs;
                      rs << "Schematic decoded from anomaly: ";
                      for (size_t i = 0; i < unlocked_now.size(); ++i) {
                        const auto itc = content_.components.find(unlocked_now[i]);
                        const std::string cname = (itc != content_.components.end() && !itc->second.name.empty()) ? itc->second.name
                                                                                                                  : unlocked_now[i];
                        if (i) rs << ", ";
                        rs << cname;
                      }

                      const std::string nm = anom->name.empty()
                                                 ? (std::string("Anomaly ") + std::to_string(static_cast<int>(anom->id)))
                                                 : anom->name;
                      rs << " (" << nm << ")";

                      EventContext rctx;
                      rctx.system_id = ship.system_id;
                      rctx.ship_id = ship_id;
                      rctx.faction_id = ship.faction_id;
                      push_event(EventLevel::Info, EventCategory::Research, rs.str(), rctx);
                    }
                  }
                }
              }
            }
          }

          // Mineral cache reward: load into ship cargo; overflow becomes a wreck (salvage cache).
          std::unordered_map<std::string, double> minerals_loaded;
          std::unordered_map<std::string, double> minerals_overflow;
          double minerals_loaded_total = 0.0;
          double minerals_overflow_total = 0.0;
          Id cache_wreck_id = kInvalidId;

          if (!anom->mineral_reward.empty()) {
            const ShipDesign* d = find_design(ship.design_id);
            const double cap = d ? std::max(0.0, d->cargo_tons) : 0.0;
            double free = std::max(0.0, cap - cargo_used_tons(ship));

            std::vector<std::pair<std::string, double>> items;
            items.reserve(anom->mineral_reward.size());
            for (const auto& [m, t] : anom->mineral_reward) {
              if (m.empty()) continue;
              if (!(t > 1e-9) || std::isnan(t) || std::isinf(t)) continue;
              items.emplace_back(m, t);
            }
            std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
              if (a.second != b.second) return a.second > b.second;
              return a.first < b.first;
            });

            for (const auto& [m, t] : items) {
              double remaining = t;

              // Load into cargo.
              if (free > 1e-9 && cap > 1e-9) {
                const double load = std::min(remaining, free);
                if (load > 1e-9) {
                  double& cur = ship.cargo[m];
                  if (!std::isfinite(cur) || cur < 0.0) cur = 0.0;
                  cur += load;
                  minerals_loaded[m] += load;
                  minerals_loaded_total += load;
                  remaining -= load;
                  free -= load;
                }
              }

              if (remaining > 1e-9) {
                minerals_overflow[m] += remaining;
                minerals_overflow_total += remaining;
              }
            }

            // If we couldn't carry everything, drop a salvageable cache wreck at the anomaly location.
            if (!minerals_overflow.empty() && cfg_.enable_wrecks) {
              const Id wid = allocate_id(state_);
              Wreck w;
              w.id = wid;
              w.system_id = ship.system_id;
              w.position_mkm = anom->position_mkm;
              w.name = anom->name.empty()
                           ? (std::string("Salvage Cache (Anomaly ") + std::to_string(static_cast<int>(anom->id)) + ")")
                           : (std::string("Salvage Cache: ") + anom->name);
              w.kind = WreckKind::Cache;
              w.minerals = minerals_overflow;

              // This wreck represents a mineral cache (not a destroyed ship hull).
              // Clear source metadata so salvaging it cannot accidentally trigger
              // reverse-engineering of the investigating ship's design.
              w.source_ship_id = kInvalidId;
              w.source_faction_id = kInvalidId;
              w.source_design_id.clear();
              w.created_day = state_.date.days_since_epoch();
              state_.wrecks[wid] = std::move(w);
              cache_wreck_id = wid;
            }
          }

          // Anomaly hazard (non-lethal damage).
          bool hazard_triggered = false;
          double hazard_shield_dmg = 0.0;
          double hazard_hull_dmg = 0.0;
          {
            const double p = std::clamp(anom->hazard_chance, 0.0, 1.0);
            const double dmg0 = std::max(0.0, anom->hazard_damage);
            if (p > 1e-9 && dmg0 > 1e-9) {
              const std::uint64_t seed = static_cast<std::uint64_t>(anom->id) * 0x9e3779b97f4a7c15ULL ^
                                         static_cast<std::uint64_t>(ship_id) * 0xbf58476d1ce4e5b9ULL ^
                                         static_cast<std::uint64_t>(ship.faction_id) * 0x94d049bb133111ebULL;
              const double roll = u01_from_u64(splitmix64(seed));
              if (roll < p) {
                hazard_triggered = true;

                const ShipDesign* d = find_design(ship.design_id);
                const double max_hp = d ? std::max(1.0, d->max_hp) : std::max(1.0, ship.hp);
                const double max_sh = d ? std::max(0.0, d->max_shields) : std::max(0.0, ship.shields);

                ship.hp = std::clamp(ship.hp, 0.0, max_hp);
                ship.shields = std::clamp(ship.shields, 0.0, max_sh);

                double dmg = dmg0;

                hazard_shield_dmg = std::min(dmg, ship.shields);
                ship.shields -= hazard_shield_dmg;
                dmg -= hazard_shield_dmg;

                hazard_hull_dmg = std::min(dmg, std::max(0.0, ship.hp - 1.0));
                ship.hp -= hazard_hull_dmg;
                dmg -= hazard_hull_dmg;

                // Dedicated warning event (the info event below summarizes rewards).
                if (hazard_shield_dmg + hazard_hull_dmg > 1e-9) {
                  std::ostringstream hs;
                  hs.setf(std::ios::fixed);
                  hs.precision(1);

                  const std::string nm = anom->name.empty()
                                             ? (std::string("Anomaly ") + std::to_string(static_cast<int>(anom->id)))
                                             : anom->name;

                  hs << "Anomaly hazard: " << ship.name << " took " << (hazard_shield_dmg + hazard_hull_dmg)
                     << " damage while investigating " << nm;
                  if (hazard_shield_dmg > 1e-9 || hazard_hull_dmg > 1e-9) {
                    hs << " (shields -" << hazard_shield_dmg << ", hull -" << hazard_hull_dmg << ")";
                  }

                  EventContext hctx;
                  hctx.system_id = ship.system_id;
                  hctx.ship_id = ship_id;
                  hctx.faction_id = ship.faction_id;
                  push_event(EventLevel::Warn, EventCategory::Exploration, hs.str(), hctx);
                }
              }
            }
          }

          // Event.
          {
            const std::string nm = anom->name.empty()
                                       ? (std::string("Anomaly ") + std::to_string(static_cast<int>(anom->id)))
                                       : anom->name;

            std::ostringstream ss;
            ss.setf(std::ios::fixed);
            ss.precision(1);
            ss << "Anomaly investigated: " << nm;
            if (rp > 1e-9) ss << " (+" << rp << " RP)";
            if (unlocked_component && !unlocked_component_id.empty()) {
              const auto itc = content_.components.find(unlocked_component_id);
              const std::string cname = (itc != content_.components.end() && !itc->second.name.empty()) ? itc->second.name
                                                                                                         : unlocked_component_id;
              ss << "; unlocked " << cname;
            }


            // Schematic fragment summary (if any).
            if (!schematic_frags.empty()) {
              const auto& f0 = schematic_frags.front();
              const auto itc = content_.components.find(f0.component_id);
              const std::string cname = (itc != content_.components.end() && !itc->second.name.empty()) ? itc->second.name
                                                                                                         : f0.component_id;

              ss << "; schematic shard (" << procgen_obscure::theme_domain_label(schematic_domain) << "): " << cname;
              if (f0.unlocked) {
                ss << " (decoded)";
              } else if (f0.points_required > 0.0) {
                const double pct = 100.0 * std::clamp(f0.points_total / f0.points_required, 0.0, 1.0);
                ss << " (" << pct << "%)";
              }
              if (schematic_frags.size() > 1) ss << " +" << (schematic_frags.size() - 1) << " more";
            }

            // Mineral cache rewards / salvage cache.
            if (minerals_loaded_total > 1e-9 || minerals_overflow_total > 1e-9) {
              if (minerals_loaded_total > 1e-9) {
                ss << "; recovered " << minerals_loaded_total << "t minerals";
                if (cache_wreck_id != kInvalidId && minerals_overflow_total > 1e-9) {
                  ss << " (" << minerals_overflow_total << "t left as salvage cache)";
                } else if (minerals_overflow_total > 1e-9 && !cfg_.enable_wrecks) {
                  ss << " (" << minerals_overflow_total << "t lost)";
                }
              } else if (cache_wreck_id != kInvalidId && minerals_overflow_total > 1e-9) {
                ss << "; located a salvage cache (" << minerals_overflow_total << "t minerals)";
              }
            }

            // Hazard summary (details logged as a Warn event above).
            if (hazard_triggered) {
              const double hd = hazard_shield_dmg + hazard_hull_dmg;
              if (hd > 1e-9) ss << "; hazard triggered (-" << hd << " dmg)";
              else ss << "; hazard triggered";
            }

            EventContext ctx;
            ctx.system_id = ship.system_id;
            ctx.ship_id = ship_id;
            ctx.faction_id = ship.faction_id;
            push_event(EventLevel::Info, EventCategory::Exploration, ss.str(), ctx);

            // Curated journal entry for the investigating faction.
            {
              JournalEntry je;
              je.category = EventCategory::Exploration;
              je.system_id = ship.system_id;
              je.ship_id = ship_id;
              je.anomaly_id = anom ? anom->id : investigate_anom_id;
              je.title = "Anomaly Resolved: " + nm;
              std::ostringstream js;
              js << ss.str();

              // Add an "obscure" procedural fingerprint + short lore line.
              // This is deterministic from ids/kind and helps make repeated
              // anomaly investigations feel less identical.
              if (anom) {
                const auto* sys = find_ptr(state_.systems, anom->system_id);
                const auto* reg = (sys && sys->region_id != kInvalidId) ? find_ptr(state_.regions, sys->region_id) : nullptr;
                const double neb = sys ? std::clamp(sys->nebula_density, 0.0, 1.0) : 0.0;
                const double ruins = reg ? std::clamp(reg->ruins_density, 0.0, 1.0) : 0.0;
                const double pir = reg ? std::clamp(reg->pirate_risk * (1.0 - reg->pirate_suppression), 0.0, 1.0) : 0.0;

                const std::string sig = procgen_obscure::anomaly_signature_code(*anom);
                js << "\n\nSignal fingerprint: " << sig;
                js << "\n" << procgen_obscure::anomaly_signature_glyph(*anom);
                js << "\n\n" << procgen_obscure::anomaly_lore_line(*anom, neb, ruins, pir);


                if (cfg_.enable_obscure_codex_fragments) {
                  const Id root = procgen_obscure::anomaly_chain_root_id(state_.anomalies, anom->id);
                  const int req = std::max(1, cfg_.codex_fragments_required);
                  const int have = procgen_obscure::faction_resolved_anomaly_chain_count(state_.anomalies, ship.faction_id, root);
                  const double frac = std::clamp(static_cast<double>(have) / static_cast<double>(req), 0.0, 1.0);

                  js << "\n\nCodex fragment (" << have << "/" << req << " decoded)";
                  js << "\nCiphertext: " << procgen_obscure::codex_ciphertext(*anom);
                  js << "\nTranslation: " << procgen_obscure::codex_partial_plaintext(*anom, frac);
                }


                // Schematic fragments (reverse-engineering progress) detail.
                if (!schematic_frags.empty()) {
                  js << "\n\nSchematic fragments (" << procgen_obscure::theme_domain_label(schematic_domain) << " domain)";
                  if (schematic_points_total > 1e-9) {
                    js.setf(std::ios::fixed);
                    js.precision(1);
                    js << " [" << schematic_points_total << " pts]";
                  }

                  for (const auto& frag : schematic_frags) {
                    const auto itc = content_.components.find(frag.component_id);
                    const std::string cname = (itc != content_.components.end() && !itc->second.name.empty()) ? itc->second.name
                                                                                                               : frag.component_id;

                    js << "\n\n- " << cname;
                    const std::string fcode = procgen_obscure::schematic_fragment_code(*anom, frag.component_id);
                    js << "\n  Shard signature: " << fcode;
                    js << "\n" << procgen_obscure::schematic_fragment_glyph(*anom, frag.component_id);

                    if (frag.points_required > 0.0) {
                      js.setf(std::ios::fixed);
                      js.precision(1);
                      const double pct = 100.0 * std::clamp(frag.points_total / frag.points_required, 0.0, 1.0);
                      js << "\n  Progress: " << frag.points_total << "/" << frag.points_required << " (" << pct << "%)";
                    } else {
                      js.setf(std::ios::fixed);
                      js.precision(1);
                      js << "\n  Progress: +" << frag.points_added << " pts";
                    }

                    if (frag.unlocked) {
                      js << "\n  Status: Decoded (component unlocked)";
                    }
                  }
                }
              }

              // Add a breakdown of mineral rewards (when present) for readability.
              if (!minerals_loaded.empty() || !minerals_overflow.empty()) {
                js << "\n\nMinerals:";
                auto dump_map = [&](const std::unordered_map<std::string, double>& m, const char* label) {
                  if (m.empty()) return;
                  js << "\n" << label;
                  std::vector<std::pair<std::string, double>> items;
                  items.reserve(m.size());
                  for (const auto& [k, v] : m) {
                    if (k.empty() || !(v > 1e-9) || !std::isfinite(v)) continue;
                    items.emplace_back(k, v);
                  }
                  std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
                    if (a.second != b.second) return a.second > b.second;
                    return a.first < b.first;
                  });
                  js.setf(std::ios::fixed);
                  js.precision(1);
                  for (const auto& [k, v] : items) {
                    js << "\n  - " << k << ": " << v << "t";
                  }
                };
                dump_map(minerals_loaded, "Loaded into cargo:");
                if (cache_wreck_id != kInvalidId) dump_map(minerals_overflow, "Left as salvage cache:");
                else dump_map(minerals_overflow, cfg_.enable_wrecks ? "Overflow:" : "Lost (no wrecks):");
              }

              je.text = js.str();
              push_journal_entry(ship.faction_id, std::move(je));
            }
          }

          const Anomaly resolved_copy = anom ? *anom : Anomaly{};

          // Procedural exploration lead (optional follow-up site / chart / cache).
          {
            const LeadOutcome lead = maybe_spawn_anomaly_lead(*this, ship, resolved_copy);
            if (lead.kind != LeadKind::None && lead.target_system_id != kInvalidId) {
              const auto* tgt = find_ptr(state_.systems, lead.target_system_id);
              const std::string tgt_name = (tgt && !tgt->name.empty()) ? tgt->name : std::string("(unknown)");

              std::ostringstream ls;
              ls.setf(std::ios::fixed);
              ls.precision(1);

              if (lead.kind == LeadKind::StarChart) {
                ls << "Star chart recovered: route to " << tgt_name;
              } else if (lead.kind == LeadKind::HiddenCache) {
                ls << "Coordinates recovered: hidden cache in " << tgt_name;
              } else {
                ls << "Signal lead recovered: follow-up site in " << tgt_name;
              }

              if (lead.hops > 0) ls << " (" << lead.hops << " hop" << (lead.hops == 1 ? "" : "s") << ")";
              if (lead.revealed_new_system) ls << "; new system revealed";
              else if (lead.revealed_route) ls << "; route intel updated";

              EventContext lctx;
              lctx.faction_id = ship.faction_id;
              lctx.ship_id = ship_id;
              lctx.system_id = lead.target_system_id;
              push_event(EventLevel::Info, EventCategory::Exploration, ls.str(), lctx);

              // Curated journal entry.
              JournalEntry lje;
              lje.category = EventCategory::Exploration;
              lje.system_id = lead.target_system_id;
              lje.ship_id = ship_id;
              if (lead.spawned_anomaly_id != kInvalidId) lje.anomaly_id = lead.spawned_anomaly_id;
              if (lead.spawned_wreck_id != kInvalidId) lje.wreck_id = lead.spawned_wreck_id;

              const std::string kind_name = (lead.kind == LeadKind::StarChart)
                                                ? "Star Chart"
                                                : (lead.kind == LeadKind::HiddenCache) ? "Hidden Cache"
                                                                                       : "Signal Trace";
              lje.title = "Exploration Lead: " + kind_name;

              std::ostringstream jt;
              std::string source_nm;
              if (!resolved_copy.name.empty()) source_nm = resolved_copy.name;
              else source_nm = "Anomaly #" + std::to_string(resolved_copy.id);
              jt << "Source anomaly: " << source_nm;
              jt << "\nTarget system: " << tgt_name;
              if (lead.hops > 0) jt << " (" << lead.hops << " hop" << (lead.hops == 1 ? "" : "s") << ")";
              if (lead.revealed_new_system) jt << "\nIntel: new system revealed via recovered chart.";
              else if (lead.revealed_route) jt << "\nIntel: navigation route updated via recovered coordinates.";

              if (lead.spawned_anomaly_id != kInvalidId) {
                if (const auto* la = find_ptr(state_.anomalies, lead.spawned_anomaly_id)) {
                  const std::string ln = !la->name.empty() ? la->name : std::string("(unnamed anomaly)");
                  jt << "\n\nSite: " << ln;
                  if (!la->kind.empty()) jt << "\nKind: " << la->kind;
                  jt << "\nInvestigation: " << std::max(1, la->investigation_days) << " day(s) on-station";
                  if (la->research_reward > 1e-9) {
                    jt.setf(std::ios::fixed);
                    jt.precision(1);
                    jt << "\nPotential reward: +" << la->research_reward << " RP";
                  }
                  if (!la->unlock_component_id.empty()) jt << "\nPotential unlock: " << la->unlock_component_id;
                  if (!la->mineral_reward.empty()) {
                    double total = 0.0;
                    for (const auto& [_, t] : la->mineral_reward) total += std::max(0.0, t);
                    if (total > 1e-3) {
                      jt.setf(std::ios::fixed);
                      jt.precision(1);
                      jt << "\nPotential cache: " << total << "t minerals";
                    }
                  }
                }
              }

              if (lead.spawned_wreck_id != kInvalidId) {
                if (const auto* w = find_ptr(state_.wrecks, lead.spawned_wreck_id)) {
                  double total = 0.0;
                  for (const auto& [_, t] : w->minerals) total += std::max(0.0, t);
                  jt.setf(std::ios::fixed);
                  jt.precision(1);
                  jt << "\n\nCache minerals (estimated): " << total << "t";
                }
              }

              lje.text = jt.str();
              push_journal_entry(ship.faction_id, std::move(lje));
            }
          }

          // Completing enough codex fragments in a lead chain can reveal a special follow-up site.
          if (const auto codex = maybe_trigger_codex_echo(*this, ship, resolved_copy); codex.has_value()) {
            const auto* tgt = find_ptr(state_.systems, codex->target_system_id);
            const std::string tgt_name = (tgt && !tgt->name.empty()) ? tgt->name : std::string("(unknown)");

            const std::string theme = procgen_obscure::anomaly_theme_label(resolved_copy);

            std::ostringstream cs;
            cs << "Codex decoded (" << codex->fragments_have << "/" << codex->fragments_required << "): " << theme
               << " -> " << tgt_name;
            if (codex->hops > 0) cs << " (" << codex->hops << " hop" << (codex->hops == 1 ? "" : "s") << ")";
            if (codex->offered_contract_id != kInvalidId) cs << "; contract offered";

            EventContext cctx;
            cctx.faction_id = ship.faction_id;
            cctx.ship_id = ship_id;
            cctx.system_id = codex->target_system_id;
            push_event(EventLevel::Info, EventCategory::Exploration, cs.str(), cctx);

            JournalEntry cje;
            cje.category = EventCategory::Exploration;
            cje.system_id = codex->target_system_id;
            cje.ship_id = ship_id;
            cje.anomaly_id = codex->spawned_anomaly_id;
            cje.title = "Codex Decoded: " + theme;

            std::ostringstream jt;
            jt << "Fragments: " << codex->fragments_have << "/" << codex->fragments_required;
            jt << "\nSource chain: " << theme;

            jt << "\n\nCiphertext:";
            jt << "\n" << procgen_obscure::codex_ciphertext(resolved_copy);
            jt << "\n\nTranslation:";
            jt << "\n" << procgen_obscure::codex_plaintext(resolved_copy);

            jt << "\n\nCoordinates resolved: " << tgt_name;
            if (codex->hops > 0) jt << " (" << codex->hops << " hop" << (codex->hops == 1 ? "" : "s") << ")";

            if (codex->spawned_anomaly_id != kInvalidId) {
              if (const auto* ca = find_ptr(state_.anomalies, codex->spawned_anomaly_id)) {
                const std::string cn = !ca->name.empty() ? ca->name : std::string("(unnamed anomaly)");
                jt << "\n\nCodex Echo site: " << cn;
                if (!ca->kind.empty()) jt << "\nKind: " << ca->kind;
                jt << "\nInvestigation: " << std::max(1, ca->investigation_days) << " day(s) on-station";
                if (ca->research_reward > 1e-9) {
                  jt.setf(std::ios::fixed);
                  jt.precision(1);
                  jt << "\nPotential reward: +" << ca->research_reward << " RP";
                }
                if (!ca->unlock_component_id.empty()) jt << "\nPotential unlock: " << ca->unlock_component_id;
              }
            }

            if (codex->offered_contract_id != kInvalidId) {
              if (const auto* c = find_ptr(state_.contracts, codex->offered_contract_id)) {
                jt.setf(std::ios::fixed);
                jt.precision(1);
                jt << "\n\nContract offer: " << c->name;
                jt << "\nReward: +" << c->reward_research_points << " RP";
              } else {
                jt << "\n\nContract offer: available on the mission board.";
              }
            }

            cje.text = jt.str();
            push_journal_entry(ship.faction_id, std::move(cje));
          }

          q.erase(q.begin());
        }
      } else {
        // Malformed order variant; drop it.
        q.erase(q.begin());
      }

      continue;
    }

    if (is_mining_op && dist <= dock_range) {
      ship.position_mkm = target;
      const double mined = do_body_mining();
      if (mining_order_complete(mined)) q.erase(q.begin());
      continue;
    }
    if (is_troop_op && dist <= dock_range) {
      ship.position_mkm = target;

      const auto* design = find_design(ship.design_id);
      const double cap = design ? std::max(0.0, design->troop_capacity) : 0.0;

      auto* col = find_ptr(state_.colonies, troop_colony_id);
      if (!col) {
        q.erase(q.begin());
        continue;
      }

      // Troop transfers are throughput-limited (especially in sub-day tick modes).
      double landing_factor = 1.0;
      if (troop_mode == 2 && cfg_.enable_blockades) {
        const double control = invasion_orbital_control(troop_colony_id, ship.faction_id);
        constexpr double kFullControl = 0.5;
        landing_factor = (kFullControl > 1e-9) ? std::clamp(control / kFullControl, 0.0, 1.0) : 1.0;

        // If we have *no* ability to establish orbital control (e.g. no armed presence),
        // keep the order queued but don't unload troops.
        if (landing_factor <= 1e-9 && ship.troops > 1e-9) {
          const auto* fac = find_ptr(state_.factions, ship.faction_id);
          if (fac && fac->control == FactionControl::Player && state_.hour_of_day == 0) {
            std::ostringstream oss;
            oss << "Landing stalled at " << col->name << ": insufficient orbital control to disembark troops.";
            EventContext ctx;
            ctx.faction_id = ship.faction_id;
            ctx.faction_id2 = col->faction_id;
            ctx.ship_id = ship_id;
            ctx.colony_id = col->id;
            ctx.system_id = ship.system_id;
            this->push_event(EventLevel::Warn, EventCategory::Combat, oss.str(), ctx);
          }
          continue;
        }
      }

      double throughput_limit = 1e300;
      if (dt_days > 0.0) {
        const double per_cap = std::max(0.0, cfg_.troop_transfer_strength_per_day_per_troop_cap);
        const double min_rate = std::max(0.0, cfg_.troop_transfer_strength_per_day_min);
        const double rate_per_day = std::max(min_rate, cap * per_cap);
        throughput_limit = rate_per_day * dt_days * landing_factor;
      }

      auto transfer_amount = [&](double want, double available, double free_cap) -> double {
        double take = (want <= 0.0) ? 1e300 : want;
        take = std::min(take, available);
        take = std::min(take, free_cap);
        take = std::min(take, throughput_limit);
        if (take < 0.0) take = 0.0;
        return take;
      };

      constexpr double kEps = 1e-9;
      bool complete = false;

      if (troop_mode == 0) {
        // Load from colony garrison.
        double want = load_troops_ord ? load_troops_ord->strength : troop_strength;
        const double free_cap = std::max(0.0, cap - ship.troops);
        const double moved = transfer_amount(want, std::max(0.0, col->ground_forces), free_cap);
        if (moved > kEps) {
          ship.troops += moved;
          col->ground_forces = std::max(0.0, col->ground_forces - moved);
          if (auto itb = state_.ground_battles.find(col->id); itb != state_.ground_battles.end()) {
            itb->second.defender_strength = col->ground_forces;
          }
        }

        if (want <= 0.0) {
          // "As much as possible": complete when we can't move any more.
          complete = (moved <= kEps);
        } else {
          if (load_troops_ord) {
            load_troops_ord->strength = std::max(0.0, load_troops_ord->strength - moved);
            want = load_troops_ord->strength;
          }
          complete = (want <= kEps) || (moved <= kEps);
        }
      } else if (troop_mode == 1) {
        // Unload into colony garrison.
        double want = unload_troops_ord ? unload_troops_ord->strength : troop_strength;
        const double moved = transfer_amount(want, std::max(0.0, ship.troops), 1e300);
        if (moved > kEps) {
          ship.troops = std::max(0.0, ship.troops - moved);
          col->ground_forces += moved;
          if (auto itb = state_.ground_battles.find(col->id); itb != state_.ground_battles.end()) {
            itb->second.defender_strength = col->ground_forces;
          }
        }

        if (want <= 0.0) {
          complete = (moved <= kEps);
        } else {
          if (unload_troops_ord) {
            unload_troops_ord->strength = std::max(0.0, unload_troops_ord->strength - moved);
            want = unload_troops_ord->strength;
          }
          complete = (want <= kEps) || (moved <= kEps);
        }
      } else if (troop_mode == 2) {
        // Invade: disembark troops into attacker strength over time.
        if (ship.troops <= kEps) {
          complete = true;
        } else {
          const double moved = std::min(std::max(0.0, ship.troops), throughput_limit);
          if (moved > kEps) {
            ship.troops = std::max(0.0, ship.troops - moved);

            GroundBattle& b = state_.ground_battles[col->id];
            if (b.colony_id == kInvalidId) {
              b.colony_id = col->id;
              b.system_id = ship.system_id;
              b.attacker_faction_id = ship.faction_id;
              b.defender_faction_id = col->faction_id;
              b.attacker_strength = 0.0;
              b.defender_strength = std::max(0.0, col->ground_forces);
              b.fortification_damage_points = 0.0;
              b.days_fought = 0;
            }
            // Reinforcement: if attacker changes, treat as a new battle by replacing.
            if (b.attacker_faction_id != ship.faction_id) {
              b.attacker_faction_id = ship.faction_id;
              b.defender_faction_id = col->faction_id;
              b.attacker_strength = 0.0;
              b.defender_strength = std::max(0.0, col->ground_forces);
              b.fortification_damage_points = 0.0;
              b.days_fought = 0;
            }
            b.attacker_strength += moved;
          }
          complete = (ship.troops <= kEps);
        }
      } else {
        // Unknown mode.
        complete = true;
      }

      if (complete) q.erase(q.begin());
      continue;
    }

    if (is_colonist_op && dist <= dock_range) {
      ship.position_mkm = target;

      auto* col = find_ptr(state_.colonies, colonist_colony_id);
      if (!col || col->faction_id != ship.faction_id) {
        q.erase(q.begin());
        continue;
      }

      const auto* design = find_design(ship.design_id);
      const double cap = design ? std::max(0.0, design->colony_capacity_millions) : 0.0;
      if (cap <= 1e-9) {
        q.erase(q.begin());
        continue;
      }

      // Colonist transfers are throughput-limited (especially in sub-day tick modes),
      // similar to cargo/fuel/troop transfers.
      double blockade_mult = 1.0;
      if (cfg_.enable_blockades) {
        blockade_mult = std::clamp(blockade_output_multiplier_for_colony(col->id), 0.0, 1.0);
      }

      double throughput_limit = 1e300;
      if (dt_days > 0.0) {
        const double per_cap = std::max(0.0, cfg_.colonist_transfer_millions_per_day_per_colony_cap);
        const double min_rate = std::max(0.0, cfg_.colonist_transfer_millions_per_day_min);
        const double rate_per_day = std::max(min_rate, cap * per_cap);
        throughput_limit = rate_per_day * dt_days * blockade_mult;
      }

      auto transfer_amount = [&](double want, double available, double free_cap) -> double {
        double take = (want <= 0.0) ? 1e300 : want;
        take = std::min(take, available);
        take = std::min(take, free_cap);
        take = std::min(take, throughput_limit);
        if (take < 0.0) take = 0.0;
        return take;
      };

      constexpr double kEps = 1e-9;
      const bool stalled_by_blockade = (cfg_.enable_blockades && blockade_mult <= kEps);

      double moved = 0.0;
      bool complete = false;

      if (colonist_mode == 0) {
        // Load from colony population.
        double want = load_colonists_ord ? load_colonists_ord->millions : colonist_millions;

        const double ship_have = std::max(0.0, ship.colonists_millions);
        const double free_cap = std::max(0.0, cap - ship_have);
        const double avail = std::max(0.0, col->population_millions);

        moved = transfer_amount(want, avail, free_cap);
        if (moved > kEps) {
          ship.colonists_millions = ship_have + moved;
          col->population_millions = std::max(0.0, col->population_millions - moved);
        }

        if (want <= 0.0) {
          // "As much as possible": finish once we are full or the source is empty, or
          // if we can't move anything this tick.
          const double ship_after = std::max(0.0, ship.colonists_millions);
          const double free_after = std::max(0.0, cap - ship_after);
          const double avail_after = std::max(0.0, col->population_millions);

          complete = (moved <= kEps) || (free_after <= kEps) || (avail_after <= kEps);

          // If we are hard-stalled by a blockade but transfer is otherwise possible,
          // keep the order queued instead of silently completing.
          if (complete && moved <= kEps && stalled_by_blockade && free_cap > kEps && avail > kEps) {
            complete = false;
          }
        } else {
          if (load_colonists_ord) {
            load_colonists_ord->millions = std::max(0.0, load_colonists_ord->millions - moved);
            want = load_colonists_ord->millions;
          }

          complete = (want <= kEps) || (moved <= kEps);

          if (complete && moved <= kEps && stalled_by_blockade && free_cap > kEps && avail > kEps && want > kEps) {
            complete = false;
          }
        }
      } else if (colonist_mode == 1) {
        // Unload into colony population.
        double want = unload_colonists_ord ? unload_colonists_ord->millions : colonist_millions;

        const double ship_have = std::max(0.0, ship.colonists_millions);
        moved = transfer_amount(want, ship_have, 1e300);
        if (moved > kEps) {
          ship.colonists_millions = std::max(0.0, ship_have - moved);
          col->population_millions += moved;
        }

        if (want <= 0.0) {
          const double ship_after = std::max(0.0, ship.colonists_millions);
          complete = (moved <= kEps) || (ship_after <= kEps);

          if (complete && moved <= kEps && stalled_by_blockade && ship_have > kEps) {
            complete = false;
          }
        } else {
          if (unload_colonists_ord) {
            unload_colonists_ord->millions = std::max(0.0, unload_colonists_ord->millions - moved);
            want = unload_colonists_ord->millions;
          }

          complete = (want <= kEps) || (moved <= kEps);

          if (complete && moved <= kEps && stalled_by_blockade && ship_have > kEps && want > kEps) {
            complete = false;
          }
        }
      } else {
        // Unknown mode.
        complete = true;
      }

      // Player-facing events: avoid spamming the log under sub-day ticks by emitting
      // at most once per day (hour 0) plus a final completion message.
      if (moved > kEps) {
        const auto* fac = find_ptr(state_.factions, ship.faction_id);
        const bool is_player = fac && fac->control == FactionControl::Player;
        if (is_player && (complete || state_.hour_of_day == 0)) {
          std::ostringstream ss;
          ss.setf(std::ios::fixed);
          ss.precision(2);
          if (colonist_mode == 0) {
            ss << "Ship " << ship.name << " loaded " << moved << "M colonists at colony " << col->name;
          } else {
            ss << "Ship " << ship.name << " unloaded " << moved << "M colonists at colony " << col->name;
          }
          EventContext ctx;
          ctx.faction_id = ship.faction_id;
          ctx.system_id = ship.system_id;
          ctx.ship_id = ship_id;
          ctx.colony_id = col->id;
          push_event(EventLevel::Info, EventCategory::Movement, ss.str(), ctx);
        }
      } else if (!complete && stalled_by_blockade) {
        const auto* fac = find_ptr(state_.factions, ship.faction_id);
        const bool is_player = fac && fac->control == FactionControl::Player;
        if (is_player && state_.hour_of_day == 0) {
          std::ostringstream ss;
          ss << "Colonist transfer stalled at " << col->name << ": blockade pressure prevents shuttle traffic.";
          EventContext ctx;
          ctx.faction_id = ship.faction_id;
          ctx.system_id = ship.system_id;
          ctx.ship_id = ship_id;
          ctx.colony_id = col->id;
          push_event(EventLevel::Warn, EventCategory::Movement, ss.str(), ctx);
        }
      }

      if (complete) q.erase(q.begin());
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

        // If habitability is enabled, seed "prefab" habitation infrastructure
        // so the initial colony has some life support on hostile worlds.
        //
        // This models the colony module delivering domes / life support as part
        // of the colony ship payload.
        if (cfg_.enable_habitability && cfg_.seed_habitation_on_colonize) {
          const double hab = body_habitability_for_faction(body->id, ship_snapshot.faction_id);
          if (hab < 0.999) {
            constexpr const char* kHabitationInstallationId = "infrastructure";
            auto it_install = content_.installations.find(kHabitationInstallationId);
            if (it_install != content_.installations.end()) {
              const double per_unit = it_install->second.habitation_capacity_millions;
              if (per_unit > 1e-9) {
                const double required = cap * std::clamp(1.0 - hab, 0.0, 1.0);
                const int units = static_cast<int>(std::ceil(required / per_unit));
                if (units > 0) new_col.installations[kHabitationInstallationId] = units;
              }
            }
          }
        }

        // Transfer all carried cargo minerals to the new colony.
        for (const auto& [mineral, tons] : ship_snapshot.cargo) {
          if (tons > 1e-9) new_col.minerals[mineral] += tons;
        }

        // Apply faction-level colony founding defaults (QoL automation preset).
        bool applied_founding_profile = false;
        std::string applied_profile_label;
        if (const auto* fac = find_ptr(state_.factions, ship_snapshot.faction_id)) {
          if (fac->auto_apply_colony_founding_profile) {
            const ColonyAutomationProfile& p = fac->colony_founding_profile;
            const bool has = (p.garrison_target_strength > 0.0) || !p.installation_targets.empty() ||
                             !p.mineral_reserves.empty() || !p.mineral_targets.empty();
            if (has) {
              apply_colony_profile(new_col, p);
              applied_founding_profile = true;
              applied_profile_label = !fac->colony_founding_profile_name.empty()
                                          ? fac->colony_founding_profile_name
                                          : std::string("Founding Defaults");
            }
          }
        }

        state_.colonies[new_col.id] = new_col;

        // Ensure the faction has this system discovered (also invalidates route caches if newly discovered).
        discover_system_for_faction(ship_snapshot.faction_id, body->system_id);

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
          std::string msg = "Colony established: " + final_name + " on " + body->name +
                            " (population " + ss.str() + "M)";
          if (applied_founding_profile) {
            msg += "; applied profile '" + applied_profile_label + "'";
          }
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
        ord.progress_days = std::max(0.0, ord.progress_days) + dt_days;
        while (ord.duration_days > 0 && ord.progress_days >= 1.0 - 1e-12) {
          ord.duration_days -= 1;
          ord.progress_days -= 1.0;
        }
      }
      if (ord.duration_days == 0) {
        q.erase(q.begin());
      }
      // If -1, we stay here forever (until order cancelled).
      continue;
    }

    if (!is_attack && !is_escort && !is_bombard && !is_jump && !is_survey_jump_op && !is_cargo_op && !is_salvage_op && !is_salvage_loop_op && !is_investigate_anomaly_op &&
        !is_fuel_transfer_op && !is_troop_transfer_op && !is_colonist_transfer_op && !is_troop_op && !is_colonist_op && !is_mining_op && !is_body &&
        !is_orbit && !is_scrap && dist <= arrive_eps) {
      q.erase(q.begin());
      continue;
    }

    auto transit_jump = [&](Id jump_id) {
      const auto* jp = find_ptr(state_.jump_points, jump_id);
      if (!jp || jp->system_id != ship.system_id || jp->linked_jump_id == kInvalidId) return;

      const auto* dest = find_ptr(state_.jump_points, jp->linked_jump_id);
      if (!dest) return;

      // Capture survey state *before* transit for hazard tuning.
      const bool surveyed_before = is_jump_point_surveyed_by_faction(ship.faction_id, jp->id);

      // --- Procedural transit hazards (subspace turbulence / misjumps) ---
      //
      // This integrates the jump-point phenomena field into actual gameplay.
      // Hazards are deterministic per (time, ship, jump) so outcomes are stable
      // across save/load, while still varying over time.
      bool hazard_triggered = false;
      double hazard_chance = 0.0;
      double hazard_shield_dmg = 0.0;
      double hazard_hull_dmg = 0.0;
      Vec2 misjump_delta{0.0, 0.0};
      bool subsystem_glitch = false;
      std::string glitch_subsystem;
      double glitch_delta = 0.0;
      std::string phen_sig;

      if (cfg_.enable_jump_point_phenomena && cfg_.jump_phenomena_transit_hazard_strength > 1e-9) {
        const auto phen = procgen_jump_phenomena::generate(*jp);
        phen_sig = phen.signature_code;

        // Environment coupling: storms and local nebula density make transits riskier.
        const double storm = cfg_.enable_nebula_storms ? system_storm_intensity_at(jp->system_id, jp->position_mkm) : 0.0;
        const double neb = std::clamp(system_nebula_density_at(jp->system_id, jp->position_mkm), 0.0, 1.0);

        double p = std::clamp(phen.hazard_chance01, 0.0, 1.0);
        p *= std::max(0.0, cfg_.jump_phenomena_transit_hazard_strength);

        if (surveyed_before) {
          p *= std::clamp(cfg_.jump_phenomena_hazard_surveyed_multiplier, 0.0, 1.0);
        }

        // Storms add an additional risk multiplier. Nebula density adds a small bias.
        p *= (1.0 + std::max(0.0, cfg_.jump_phenomena_storm_hazard_bonus) * storm);
        p *= (1.0 + 0.25 * neb);

        hazard_chance = std::clamp(p, 0.0, 1.0);

        // Deterministic roll keyed on time + ids.
        const std::uint64_t now = static_cast<std::uint64_t>(state_.date.days_since_epoch());
        const std::uint64_t hr = static_cast<std::uint64_t>(std::clamp(state_.hour_of_day, 0, 23));
        std::uint64_t seed = now * 0x9e3779b97f4a7c15ULL ^ (hr + 1ULL) * 0xbf58476d1ce4e5b9ULL;
        seed ^= static_cast<std::uint64_t>(ship_id) * 0x94d049bb133111ebULL;
        seed ^= static_cast<std::uint64_t>(jp->id) * 0xD1B54A32D192ED03ULL;
        seed ^= static_cast<std::uint64_t>(dest->id) * 0xA24BAED4963EE407ULL;

        HashRng rng(splitmix64(seed));

        if (rng.next_u01() < hazard_chance) {
          hazard_triggered = true;

          // --- Non-lethal damage (shields first) ---
          const ShipDesign* d = find_design(ship.design_id);
          const double max_hp = d ? std::max(1.0, d->max_hp) : std::max(1.0, ship.hp);
          const double max_sh = d ? std::max(0.0, d->max_shields) : std::max(0.0, ship.shields);

          ship.hp = std::clamp(ship.hp, 0.0, max_hp);
          ship.shields = std::clamp(ship.shields, 0.0, max_sh);

          double dmg = std::max(0.0, phen.hazard_damage_frac) * max_hp;
          dmg *= std::max(0.0, cfg_.jump_phenomena_transit_hazard_strength);
          dmg *= (0.85 + 0.30 * rng.next_u01());
          dmg *= (1.0 + 0.35 * storm + 0.25 * neb);

          // Cap to avoid excessive spike damage.
          dmg = std::clamp(dmg, 0.0, std::max(0.5, 0.35 * max_hp));

          hazard_shield_dmg = std::min(dmg, ship.shields);
          ship.shields -= hazard_shield_dmg;
          dmg -= hazard_shield_dmg;

          hazard_hull_dmg = std::min(dmg, std::max(0.0, ship.hp - 1.0));
          ship.hp -= hazard_hull_dmg;
          dmg -= hazard_hull_dmg;

          // --- Misjump (emergence scatter) ---
          const double mis_strength = std::max(0.0, cfg_.jump_phenomena_misjump_strength);
          if (mis_strength > 1e-9) {
            double mp = std::clamp(0.05 + 0.35 * phen.shear01 + 0.15 * phen.turbulence01, 0.0, 1.0);
            mp *= 0.35 * mis_strength;  // default: rare unless very sheared
            if (surveyed_before) {
              mp *= std::clamp(cfg_.jump_phenomena_hazard_surveyed_multiplier, 0.0, 1.0);
            }
            mp = std::clamp(mp, 0.0, 1.0);

            if (rng.next_u01() < mp) {
              constexpr double kPi = 3.141592653589793238462643383279502884;
              double R = std::max(0.0, phen.misjump_dispersion_mkm) * mis_strength;

              // Surveying also reduces the scale of the misjump.
              if (surveyed_before) {
                R *= std::clamp(cfg_.jump_phenomena_hazard_surveyed_multiplier, 0.0, 1.0);
              }

              // Scale by local severity.
              const double sev = std::clamp(0.45 * phen.turbulence01 + 0.55 * phen.shear01, 0.0, 1.0);
              R *= (0.35 + 0.85 * sev);

              const double ang = rng.range(0.0, 2.0 * kPi);
              const double rad = std::sqrt(rng.next_u01()) * R;
              misjump_delta = Vec2{std::cos(ang) * rad, std::sin(ang) * rad};
            }
          }

          // --- Subsystem glitch (integrity hit) ---
          const double glitch_strength = std::max(0.0, cfg_.jump_phenomena_subsystem_glitch_strength);
          if (glitch_strength > 1e-9) {
            double gp = std::clamp(phen.subsystem_glitch_chance01, 0.0, 1.0);
            gp *= 0.25 * glitch_strength;  // default: uncommon
            if (surveyed_before) {
              gp *= std::clamp(cfg_.jump_phenomena_hazard_surveyed_multiplier, 0.0, 1.0);
            }
            gp = std::clamp(gp, 0.0, 1.0);

            if (rng.next_u01() < gp) {
              subsystem_glitch = true;
              const double sev = std::clamp(phen.subsystem_glitch_severity01, 0.0, 1.0);
              glitch_delta = sev * 0.30 * glitch_strength;
              glitch_delta *= (0.70 + 0.60 * rng.next_u01());
              glitch_delta = std::clamp(glitch_delta, 0.0, 0.65);

              const int which = rng.range_int(0, 3);
              auto apply = [&](double& integrity, const char* label) {
                const double before = std::clamp(integrity, 0.0, 1.0);
                integrity = std::clamp(before - glitch_delta, 0.05, 1.0);
                glitch_delta = before - integrity;  // actual applied
                glitch_subsystem = label;
              };

              switch (which) {
                case 0: apply(ship.engines_integrity, "Engines"); break;
                case 1: apply(ship.sensors_integrity, "Sensors"); break;
                case 2: apply(ship.weapons_integrity, "Weapons"); break;
                default: apply(ship.shields_integrity, "Shields"); break;
              }
            }
          }
        }
      }

      // Mark both ends as surveyed for the transiting faction (fog-of-war routing).
      survey_jump_point_for_faction(ship.faction_id, jp->id);
      survey_jump_point_for_faction(ship.faction_id, dest->id);

      const Id old_sys = ship.system_id;
      const Id new_sys = dest->system_id;

      if (auto* sys_old = find_ptr(state_.systems, old_sys)) {
        sys_old->ships.erase(std::remove(sys_old->ships.begin(), sys_old->ships.end(), ship_id), sys_old->ships.end());
      }

      ship.system_id = new_sys;
      ship.position_mkm = dest->position_mkm + misjump_delta;

      if (auto* sys_new = find_ptr(state_.systems, new_sys)) {
        sys_new->ships.push_back(ship_id);
      }

      discover_system_for_faction(ship.faction_id, new_sys);

      {
        const auto* fac = find_ptr(state_.factions, ship.faction_id);
        const bool suppress_movement_event = (fac && fac->control == FactionControl::AI_Passive);
        if (suppress_movement_event) return;

        const auto* sys_new = find_ptr(state_.systems, new_sys);
        const std::string dest_name = sys_new ? sys_new->name : std::string("(unknown)");
        const std::string msg = "Ship " + ship.name + " transited jump point " + jp->name + " -> " + dest_name;
        nebula4x::log::info(msg);
        EventContext ctx;
        ctx.faction_id = ship.faction_id;
        ctx.system_id = new_sys;
        ctx.ship_id = ship_id;
        push_event(EventLevel::Info, EventCategory::Movement, msg, ctx);

        if (hazard_triggered) {
          std::ostringstream hs;
          hs.setf(std::ios::fixed);
          hs.precision(1);

          hs << "Jump turbulence: " << ship.name;
          if (!phen_sig.empty()) hs << " (" << phen_sig << ")";

          const double d = hazard_shield_dmg + hazard_hull_dmg;
          if (d > 1e-9) {
            hs << " took " << d << " damage";
            hs << " (shields -" << hazard_shield_dmg << ", hull -" << hazard_hull_dmg << ")";
          } else {
            hs << " encountered a transit hazard";
          }

          const double md = std::sqrt(misjump_delta.x * misjump_delta.x + misjump_delta.y * misjump_delta.y);
          if (md > 1e-6) {
            hs << "; misjumped " << md << " mkm off-course";
          }

          if (subsystem_glitch && !glitch_subsystem.empty() && glitch_delta > 1e-6) {
            hs << "; " << glitch_subsystem << " integrity -" << glitch_delta;
          }

          // Helpful for debugging and for later UI layering (risk readouts).
          if (hazard_chance > 1e-6) {
            hs.setf(std::ios::fixed);
            hs.precision(0);
            hs << " (risk " << std::clamp(hazard_chance, 0.0, 1.0) * 100.0 << "%)";
          }

          EventContext hctx;
          hctx.faction_id = ship.faction_id;
          hctx.system_id = new_sys;
          hctx.ship_id = ship_id;
          push_event(EventLevel::Warn, EventCategory::Movement, hs.str(), hctx);
        }
      }
    };

    if (is_survey_jump_op && dist <= dock_range) {
      ship.position_mkm = target;

      const auto* jp = find_ptr(state_.jump_points, survey_jump_id);
      if (!jp || jp->system_id != ship.system_id) {
        q.erase(q.begin());
        continue;
      }

      // If surveying is configured as instant, mark it now so the order can complete immediately.
      if (cfg_.jump_survey_points_required <= 1e-9) {
        survey_jump_point_for_faction(ship.faction_id, survey_jump_id);
      }

      const bool surveyed = is_jump_point_surveyed_by_faction(ship.faction_id, survey_jump_id);
      if (surveyed) {
        if (survey_transit_when_done) {
          transit_jump(survey_jump_id);
        }
        q.erase(q.begin());
      }
      continue;
    }

    if (is_jump && dist <= dock_range) {
      ship.position_mkm = target;
      if (!is_coordinated_jump_group || allow_jump_transit) {
        const Id jump_id = escort_is_jump_leg ? escort_jump_id : std::get<TravelViaJump>(q.front()).jump_point_id;
        transit_jump(jump_id);
        if (!escort_is_jump_leg) q.erase(q.begin());
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
          // Lost-contact pursuit is a *search* operation; do not complete the
          // order just because we've reached one candidate search point.
          ship.position_mkm = target;
          continue;
        }
      }
    }

    if (is_bombard) {
      if (dist <= desired_range + 1e-9) {
        continue;
      }
    }

    if (is_escort && !escort_is_jump_leg) {
      if (dist <= desired_range + 1e-9) {
        continue;
      }
    }

    const auto* sd = find_design(ship.design_id);

    // Power gating: if engines draw power and the ship can't allocate it, it
    // cannot move this tick.
    double effective_speed_km_s = ship.speed_km_s;
    if (sd) {
      const auto p = compute_power_allocation(*sd, ship.power_policy);
      if (!p.engines_online) effective_speed_km_s = 0.0;
    }

    effective_speed_km_s *= maintenance_speed_mult(ship);
    effective_speed_km_s *= ship_heat_speed_multiplier(ship);
    effective_speed_km_s *= ship_subsystem_engine_multiplier(ship);

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


    // Environmental movement modifiers (nebula drag / storms).
    // With nebula microfields enabled, this is position-dependent.
    effective_speed_km_s *= this->system_movement_speed_multiplier_at(ship.system_id, ship.position_mkm);

    const double max_step = mkm_per_day_from_speed(effective_speed_km_s, cfg_.seconds_per_day) * dt_days;
    if (max_step <= 0.0) continue;

    double step = max_step;
    if (is_attack || is_bombard || (is_escort && !escort_is_jump_leg)) {
      step = std::min(step, std::max(0.0, dist - desired_range));
      if (step <= 0.0) continue;
    }

    const double fuel_cap = sd ? std::max(0.0, sd->fuel_capacity_tons) : 0.0;
    const double fuel_use = sd ? std::max(0.0, sd->fuel_use_per_mkm) : 0.0;
    // Civilian / ambient ships (AI_Passive) abstract fuel usage to avoid
    // requiring a full civilian-economy refuel loop.
    const auto* fac = find_ptr(state_.factions, ship.faction_id);
    const bool is_civilian = (fac && fac->control == FactionControl::AI_Passive);
    const bool uses_fuel = (fuel_use > 0.0) && !is_civilian;
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
          const Id jump_id = escort_is_jump_leg ? escort_jump_id : std::get<TravelViaJump>(q.front()).jump_point_id;
          transit_jump(jump_id);
          if (!escort_is_jump_leg) q.erase(q.begin());
        }
      } else if (is_attack) {
        // AttackShip remains active while pursuing a lost contact; completion is
        // governed by staleness checks rather than reaching a single search point.
      } else if (is_bombard) {
        // Bombardment executes in tick_combat; keep the order.
      } else if (is_cargo_op) {
        const double moved = do_cargo_transfer();
        if (cargo_order_complete(moved)) q.erase(q.begin());
      } else if (is_salvage_op) {
        const double moved = do_wreck_salvage();
        if (salvage_order_complete(moved)) q.erase(q.begin());
      } else if (is_salvage_loop_op) {
        process_salvage_loop_docked();
      } else if (is_mining_op) {
        const double mined = do_body_mining();
        if (mining_order_complete(mined)) q.erase(q.begin());
      } else if (is_fuel_transfer_op) {
        const double moved = do_fuel_transfer();
        if (fuel_order_complete(moved)) q.erase(q.begin());
      } else if (is_troop_transfer_op) {
        const double moved = do_troop_transfer();
        if (troop_transfer_order_complete(moved)) q.erase(q.begin());
      } else if (is_colonist_transfer_op) {
        const double moved = do_colonist_transfer();
        if (colonist_transfer_order_complete(moved)) q.erase(q.begin());
      } else if (is_troop_op) {
        // Don't pop here; troop orders execute in the dock-range check above.
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

    Vec2 dir = delta.normalized();

    // Experimental: terrain-aware navigation.
    //
    // Ships can optionally "ray-probe" a small fan of candidate headings around
    // the direct-to-target vector and pick the one with the lowest estimated
    // travel-time cost through nebula microfields / storm cells.
    //
    // This is intentionally a lightweight receding-horizon controller: it does
    // not create persistent waypoints and therefore remains robust to moving
    // targets (escort/attack) and to coarse time steps.
    if (cfg_.enable_terrain_aware_navigation && dist > std::max(arrive_eps, 1e-6) * 4.0) {
      const auto* sys = find_ptr(state_.systems, ship.system_id);
      if (sys) {
        const bool micro = (cfg_.enable_nebula_drag && cfg_.enable_nebula_microfields && sys->nebula_density > 1e-6 &&
                            cfg_.nebula_microfield_strength > 1e-9 &&
                            cfg_.nebula_drag_speed_penalty_at_max_density > 1e-9);
        const bool storm_cells = (cfg_.enable_nebula_storms && cfg_.enable_nebula_storm_cells &&
                                  system_has_storm(ship.system_id) && cfg_.nebula_storm_cell_strength > 1e-9 &&
                                  cfg_.nebula_storm_speed_penalty > 1e-9);

        // Only steer when the environment has meaningful *spatial* variation.
        if (micro || storm_cells) {
          const double strength = std::clamp(cfg_.terrain_nav_strength, 0.0, 1.0);
          const int rays = std::clamp(cfg_.terrain_nav_rays, 3, 31);
          const double max_ang = deg_to_rad(std::clamp(cfg_.terrain_nav_max_angle_deg, 1.0, 89.0));
          const double lookahead = std::clamp(cfg_.terrain_nav_lookahead_mkm, 25.0, 20000.0);
          const double turn_pen = std::max(0.0, cfg_.terrain_nav_turn_penalty);

          // Limit lookahead so we don't over-fit to very local structure when
          // closing on a target.
          const double L = std::min(dist, lookahead);

          // Deterministic per-ship/per-tick seed for tie-breaking and jitter decorrelation.
          auto qpos = [](double v) -> std::int64_t { return static_cast<std::int64_t>(std::llround(v * 16.0)); };
          std::uint64_t seed = splitmix64(static_cast<std::uint64_t>(state_.date.days_since_epoch()));
          seed = splitmix64(seed ^ static_cast<std::uint64_t>(state_.hour_of_day));
          seed = splitmix64(seed ^ static_cast<std::uint64_t>(ship_id));
          seed = splitmix64(seed ^ static_cast<std::uint64_t>(ship.system_id));
          seed = splitmix64(seed ^ (static_cast<std::uint64_t>(qpos(ship.position_mkm.x)) * 0x9E3779B97F4A7C15ULL));
          seed = splitmix64(seed ^ (static_cast<std::uint64_t>(qpos(ship.position_mkm.y)) * 0xBF58476D1CE4E5B9ULL));
          seed = splitmix64(seed ^ (static_cast<std::uint64_t>(qpos(target.x)) * 0x94D049BB133111EBULL));
          seed = splitmix64(seed ^ (static_cast<std::uint64_t>(qpos(target.y)) * 0xD6E8FEB86659FD93ULL));

          auto tiny_noise = [&](int i) -> double {
            const std::uint64_t h = splitmix64(seed ^ (0xA5A5A5A5A5A5A5A5ULL + static_cast<std::uint64_t>(i)));
            return u01_from_u64(h) * 1e-6;
          };

          // Evaluate candidate headings.
          Vec2 best_dir = dir;
          double best_score = std::numeric_limits<double>::infinity();

          const int n = rays;
          for (int i = 0; i < n; ++i) {
            const double t = (n <= 1) ? 0.0 : (static_cast<double>(i) / static_cast<double>(n - 1)) * 2.0 - 1.0;
            const double ang = t * max_ang;
            const Vec2 cand = rotate_vec2(dir, ang).normalized();

            // Don't consider headings that would move away from the goal.
            const double forward = dot(cand, dir);
            if (forward <= 1e-4) continue;

            const Vec2 end = ship.position_mkm + cand * L;

            // Estimated local travel-time cost (environment-adjusted distance).
            const double local_cost = this->system_movement_environment_cost_los(
                ship.system_id, ship.position_mkm, end, seed ^ (0xC0DEC0FFEEULL + static_cast<std::uint64_t>(i)));

            // Heuristic remainder: straight-line to goal from the ray endpoint.
            const Vec2 rem = target - end;
            const double rem_len = rem.length();
            const double m_end = std::clamp(this->system_movement_speed_multiplier_at(ship.system_id, end), 0.05, 1.0);
            const double rem_cost = rem_len / m_end;

            double score = local_cost + rem_cost;
            score *= (1.0 + turn_pen * (1.0 - forward));
            score += tiny_noise(i);

            if (score < best_score) {
              best_score = score;
              best_dir = cand;
            }
          }

          if (std::isfinite(best_score)) {
            // Blend for smoothness.
            const Vec2 blended = Vec2{dir.x * (1.0 - strength) + best_dir.x * strength,
                                      dir.y * (1.0 - strength) + best_dir.y * strength};
            const Vec2 nd = blended.normalized();
            if (nd.length_squared() > 1e-12) dir = nd;
          }
        }
      }
    }

    ship.position_mkm += dir * step;
    burn_fuel(step);
  }

  // Jump-point surveys: ships record nearby jump points for fog-of-war routing.
  //
  // Surveys are modeled as an incremental process: ships contribute "survey points"
  // over time while within range of a jump point. When progress reaches
  // cfg_.jump_survey_points_required, the jump point becomes surveyed for the
  // faction (and is shared with mutual-friendly factions).
  //
  // Setting cfg_.jump_survey_points_required <= 0 keeps the legacy instant behavior.
  const double base_required_points = cfg_.jump_survey_points_required;
  const double ref_range = std::max(1e-9, cfg_.jump_survey_reference_sensor_range_mkm);
  const double range_frac = std::max(0.0, cfg_.jump_survey_range_sensor_fraction);
  const double cap_points_per_day = std::max(0.0, cfg_.jump_survey_points_per_day_cap);

  for (Id ship_id : ship_ids) {
    const auto* sh = find_ptr(state_.ships, ship_id);
    if (!sh) continue;
    if (sh->hp <= 0.0) continue;
    if (sh->faction_id == kInvalidId) continue;
    if (sh->system_id == kInvalidId) continue;

    auto* fac = find_ptr(state_.factions, sh->faction_id);
    if (!fac) continue;

    const auto* sys = find_ptr(state_.systems, sh->system_id);
    if (!sys) continue;

    const auto* sd = find_design(sh->design_id);
    if (!sd) continue;

    // Environmental sensor attenuation (match simulation_sensors).
    const double env_mult = this->system_sensor_environment_multiplier(sh->system_id);

    // Need online sensors to contribute.
    double sensor_mkm = sim_sensors::sensor_range_mkm_with_mode(*this, *sh, *sd);
    sensor_mkm *= env_mult;
    if (sensor_mkm <= 1e-9) continue;

    // Range check: non-surveyors must be at docking range; surveyors can contribute
    // at longer range (a fraction of their effective sensor range).
    double range_mkm = std::max(0.0, cfg_.docking_range_mkm);
    if (sd->role == ShipRole::Surveyor) {
      range_mkm = std::max(range_mkm, sensor_mkm * range_frac);
    }
    if (range_mkm <= 0.0) continue;

    // Legacy: instant surveying.
    if (base_required_points <= 1e-9) {
      for (Id jid : sys->jump_points) {
        if (jid == kInvalidId) continue;
        if (is_jump_point_surveyed_by_faction(sh->faction_id, jid)) continue;
        const auto* jp = find_ptr(state_.jump_points, jid);
        if (!jp) continue;
        const double dist = (sh->position_mkm - jp->position_mkm).length();
        if (dist <= range_mkm + 1e-9) {
          survey_jump_point_for_faction(sh->faction_id, jid);
        }
      }
      continue;
    }

    // Survey rate for this ship.
    const double role_mult = (sd->role == ShipRole::Surveyor) ? cfg_.jump_survey_strength_multiplier_surveyor
                                                             : cfg_.jump_survey_strength_multiplier_other;
    double points_per_day = (sensor_mkm / ref_range) * std::max(0.0, role_mult);
    if (cap_points_per_day > 0.0) {
      points_per_day = std::clamp(points_per_day, 0.0, cap_points_per_day);
    }
    const double delta_points = points_per_day * dt_days;
    if (delta_points <= 1e-12) continue;

    // Pick the nearest unsurveyed jump point in range and apply progress to it.
    Id best_jid = kInvalidId;
    double best_dist = std::numeric_limits<double>::infinity();
    for (Id jid : sys->jump_points) {
      if (jid == kInvalidId) continue;
      if (is_jump_point_surveyed_by_faction(sh->faction_id, jid)) continue;
      const auto* jp = find_ptr(state_.jump_points, jid);
      if (!jp) continue;
      const double dist = (sh->position_mkm - jp->position_mkm).length();
      if (dist <= range_mkm + 1e-9 && dist < best_dist) {
        best_dist = dist;
        best_jid = jid;
      }
    }
    if (best_jid == kInvalidId) continue;

    double& prog = fac->jump_survey_progress[best_jid];
    if (!std::isfinite(prog) || prog < 0.0) prog = 0.0;
    prog += delta_points;

    const double required_points = this->jump_survey_required_points_for_jump(best_jid);
    if (required_points <= 1e-9 || prog >= required_points - 1e-9) {
      // Keep progress maps tidy; survey_jump_point_for_faction() will also clear.
      fac->jump_survey_progress.erase(best_jid);
      survey_jump_point_for_faction(sh->faction_id, best_jid);
    }
  }

  // --- velocity tracking ---
  //
  // Compute in-system velocity vectors for the next combat tick based on
  // position deltas over this dt.
  //
  // Ships that changed systems (jump transit) are assigned zero velocity to
  // avoid nonsensical values.
  if (dt_days > 1e-12) {
    const double inv_dt = 1.0 / dt_days;
    for (Id sid : ship_ids) {
      auto* sh = find_ptr(state_.ships, sid);
      if (!sh) continue;
      const auto itp = pre_pos_mkm.find(sid);
      const auto its = pre_sys.find(sid);
      if (itp == pre_pos_mkm.end() || its == pre_sys.end()) {
        sh->velocity_mkm_per_day = Vec2{0.0, 0.0};
        continue;
      }
      if (its->second != sh->system_id) {
        sh->velocity_mkm_per_day = Vec2{0.0, 0.0};
        continue;
      }
      const Vec2 delta = sh->position_mkm - itp->second;
      sh->velocity_mkm_per_day = delta * inv_dt;
      if (!std::isfinite(sh->velocity_mkm_per_day.x) || !std::isfinite(sh->velocity_mkm_per_day.y)) {
        sh->velocity_mkm_per_day = Vec2{0.0, 0.0};
      }
    }
  } else {
    for (Id sid : ship_ids) {
      auto* sh = find_ptr(state_.ships, sid);
      if (!sh) continue;
      sh->velocity_mkm_per_day = Vec2{0.0, 0.0};
    }
  }

}



} // namespace nebula4x
