#include "nebula4x/core/simulation.h"

#include "simulation_internal.h"
#include "simulation_procgen.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "nebula4x/core/procgen_obscure.h"
#include "nebula4x/util/trace_events.h"

namespace nebula4x {
namespace {
using sim_internal::sorted_keys;

// Small helper: clamp into [0,1] while handling NaNs.
static double clamp01(double v) {
  if (!std::isfinite(v)) return 0.0;
  return std::clamp(v, 0.0, 1.0);
}

struct RegionFactors {
  double ruins{0.0};
  double pirate{0.0};
  double salvage_mult{1.0};
};

static RegionFactors region_factors_for_system(const GameState& s, const StarSystem& sys) {
  RegionFactors f;
  if (sys.region_id == kInvalidId) return f;
  const auto* reg = find_ptr(s.regions, sys.region_id);
  if (!reg) return f;
  f.ruins = clamp01(reg->ruins_density);
  f.pirate = clamp01(reg->pirate_risk);
  f.salvage_mult = std::max(0.0, reg->salvage_richness_mult);
  return f;
}

// Deterministic per-(day,system,tag) seed.
static std::uint64_t poi_seed(std::int64_t day, Id system_id, std::uint64_t tag) {
  std::uint64_t s = static_cast<std::uint64_t>(day);
  s ^= (static_cast<std::uint64_t>(system_id) + 0x9e3779b97f4a7c15ULL) * 0xbf58476d1ce4e5b9ULL;
  s ^= tag * 0x94d049bb133111ebULL;
  return sim_procgen::splitmix64(s);
}

static bool system_has_colony(const std::unordered_set<Id>& colony_systems, Id system_id) {
  return colony_systems.find(system_id) != colony_systems.end();
}

static double min_dist_to(const Vec2& p, const std::vector<Vec2>& occupied) {
  double best = 1e30;
  for (const auto& q : occupied) {
    const double d = (p - q).length();
    if (d < best) best = d;
  }
  if (!std::isfinite(best)) return 0.0;
  return best;
}

static Vec2 pick_site_near_jump(const GameState& s, Id system_id, sim_procgen::HashRng& rng,
                               double r_min_mkm, double r_max_mkm) {
  const auto* sys = find_ptr(s.systems, system_id);
  if (!sys) return Vec2{0.0, 0.0};

  r_min_mkm = std::max(0.0, r_min_mkm);
  r_max_mkm = std::max(r_min_mkm, r_max_mkm);

  Vec2 base{0.0, 0.0};
  if (!sys->jump_points.empty()) {
    std::vector<Id> jps = sys->jump_points;
    std::sort(jps.begin(), jps.end());
    const int idx = rng.range_int(0, static_cast<int>(jps.size()) - 1);
    if (const auto* jp = find_ptr(s.jump_points, jps[static_cast<std::size_t>(idx)])) base = jp->position_mkm;
  }

  constexpr double kTwoPi = 6.2831853071795864769;
  const double ang = rng.range(0.0, kTwoPi);
  const double r = rng.range(r_min_mkm, r_max_mkm);
  return base + Vec2{std::cos(ang) * r, std::sin(ang) * r};
}

} // namespace

void Simulation::tick_dynamic_points_of_interest() {
  NEBULA4X_TRACE_SCOPE("tick_dynamic_points_of_interest", "sim");

  if (!cfg_.enable_dynamic_poi_spawns) return;
  if (state_.systems.empty()) return;

  const std::int64_t now_day = state_.date.days_since_epoch();
  const int num_systems = static_cast<int>(state_.systems.size());

  const int max_anoms_total = (cfg_.dynamic_poi_max_unresolved_anomalies_total > 0)
                                 ? cfg_.dynamic_poi_max_unresolved_anomalies_total
                                 : std::max(12, num_systems * 2);
  const int max_caches_total = (cfg_.dynamic_poi_max_active_caches_total > 0)
                                  ? cfg_.dynamic_poi_max_active_caches_total
                                  : std::max(6, num_systems);

  const int per_sys_anom_cap = std::max(0, cfg_.dynamic_poi_max_unresolved_anomalies_per_system);
  const int per_sys_cache_cap = std::max(0, cfg_.dynamic_poi_max_active_caches_per_system);

  double base_anom_chance = std::clamp(cfg_.dynamic_anomaly_spawn_chance_per_system_per_day, 0.0, 1.0);
  const double base_cache_chance = std::clamp(cfg_.dynamic_cache_spawn_chance_per_system_per_day, 0.0, 1.0);

  if (base_anom_chance <= 1e-12 && base_cache_chance <= 1e-12) return;

  // Track which systems host colonies (used to bias spawns toward unexplored space).
  std::unordered_set<Id> colony_systems;
  colony_systems.reserve(state_.colonies.size() * 2 + 8);
  for (const auto& [cid, c] : state_.colonies) {
    (void)cid;
    const auto* body = find_ptr(state_.bodies, c.body_id);
    if (!body) continue;
    if (body->system_id == kInvalidId) continue;
    colony_systems.insert(body->system_id);
  }

  // Current unresolved anomaly counts + per-system occupied POI positions.
  int unresolved_total = 0;
  std::unordered_map<Id, int> anoms_per_sys;
  anoms_per_sys.reserve(state_.anomalies.size() * 2 + 8);

  std::unordered_map<Id, std::vector<Vec2>> occupied_by_sys;
  occupied_by_sys.reserve(state_.systems.size() * 2 + 8);

  for (const auto& [aid, a] : state_.anomalies) {
    (void)aid;
    if (a.system_id == kInvalidId) continue;
    if (a.resolved) continue;
    ++unresolved_total;
    ++anoms_per_sys[a.system_id];
    occupied_by_sys[a.system_id].push_back(a.position_mkm);
  }

  // Current cache counts.
  int caches_total = 0;
  std::unordered_map<Id, int> caches_per_sys;
  caches_per_sys.reserve(state_.wrecks.size() * 2 + 8);
  for (const auto& [wid, w] : state_.wrecks) {
    (void)wid;
    if (w.system_id == kInvalidId) continue;
    if (w.kind != WreckKind::Cache) continue;
    if (w.minerals.empty()) continue;
    ++caches_total;
    ++caches_per_sys[w.system_id];
    occupied_by_sys[w.system_id].push_back(w.position_mkm);
  }

  int resolved_anomaly_count = 0;
  for (const auto& [_, a] : state_.anomalies) {
    if (a.resolved) ++resolved_anomaly_count;
  }
  int max_discovered_systems = 0;
  for (const auto& [_, f] : state_.factions) {
    max_discovered_systems = std::max(max_discovered_systems, static_cast<int>(f.discovered_systems.size()));
  }

  const double resolved_maturity = std::clamp(static_cast<double>(resolved_anomaly_count) / 42.0, 0.0, 1.0);
  const double reach_maturity = std::clamp(static_cast<double>(std::max(0, max_discovered_systems - 1)) / 16.0, 0.0, 1.0);
  const double early_exploration_pressure =
      std::clamp(0.60 * (1.0 - resolved_maturity) + 0.40 * (1.0 - reach_maturity), 0.0, 1.0);

  // Early-game exploration acceleration: spawn slightly more anomalies while the
  // galaxy is still mostly unknown.
  base_anom_chance = std::clamp(base_anom_chance * (1.0 + 0.34 * early_exploration_pressure), 0.0, 1.0);

  if (unresolved_total >= max_anoms_total && caches_total >= max_caches_total) return;

  auto neb_at = [&](Id system_id, const Vec2& pos_mkm) -> double {
    return clamp01(this->system_nebula_density_at(system_id, pos_mkm));
  };

  auto grad_at = [&](Id system_id, const Vec2& pos_mkm) -> double {
    // Local microfield gradient proxy. In clear systems this collapses to ~0.
    const double d = 24.0;
    const Vec2 dx{d, 0.0};
    const Vec2 dy{0.0, d};

    const double x1 = neb_at(system_id, pos_mkm + dx);
    const double x0 = neb_at(system_id, pos_mkm - dx);
    const double y1 = neb_at(system_id, pos_mkm + dy);
    const double y0 = neb_at(system_id, pos_mkm - dy);

    const double gx = std::abs(x1 - x0);
    const double gy = std::abs(y1 - y0);

    // 0..1-ish. Scale a bit so small gradients still matter.
    const double g = std::clamp(0.75 * (0.5 * (gx + gy)), 0.0, 1.0);
    if (!std::isfinite(g)) return 0.0;
    return g;
  };

  auto pick_biased_site = [&](Id system_id,
                              sim_procgen::HashRng& rng,
                              const std::vector<Vec2>& occupied,
                              double target_density01,
                              double w_density,
                              double w_grad,
                              double w_sep,
                              double min_sep_mkm,
                              int samples,
                              double r_min_mkm,
                              double r_max_mkm) -> Vec2 {
    samples = std::clamp(samples, 1, 64);
    min_sep_mkm = std::max(0.0, min_sep_mkm);

    Vec2 best = pick_site_near_jump(state_, system_id, rng, r_min_mkm, r_max_mkm);
    double best_score = -1e30;

    for (int i = 0; i < samples; ++i) {
      const Vec2 cand = pick_site_near_jump(state_, system_id, rng, r_min_mkm, r_max_mkm);
      const double d = neb_at(system_id, cand);
      const double g = grad_at(system_id, cand);
      const double sep = occupied.empty() ? 1e9 : min_dist_to(cand, occupied);

      // Density score peaks when close to target.
      double ds = 1.0 - std::abs(d - target_density01) / 0.35;
      ds = std::clamp(ds, 0.0, 1.0);

      // Separation: soft Poisson disk (don't hard-reject, just score).
      double ss = 1.0;
      if (min_sep_mkm > 1e-6) {
        ss = std::clamp(sep / min_sep_mkm, 0.0, 2.0);
      }

      const double score = w_density * ds + w_grad * g + w_sep * ss + 0.01 * rng.next_u01();
      if (score > best_score) {
        best_score = score;
        best = cand;
      }
    }

    return best;
  };

  auto spawn_anomaly = [&](Id system_id, const StarSystem& sys, const RegionFactors& rf) {
    Anomaly a;
    a.id = allocate_id(state_);
    a.system_id = system_id;

    auto& occupied = occupied_by_sys[system_id];

    sim_procgen::HashRng rng(poi_seed(now_day, system_id, 0xA11A11A1ULL));

    const double neb_base = clamp01(sys.nebula_density);

    // Choose a flavor (kind/name) influenced by region factors.
    // We keep this decision system-level (neb_base) so regions retain identity
    // even when microfields add local pockets.
    const double w_ruins = 0.20 + 1.40 * rf.ruins + 0.26 * early_exploration_pressure;
    const double w_distress = 0.10 + 1.10 * rf.pirate + 0.22 * early_exploration_pressure;

    // Microfield roughness estimate: filaments increase the odds of "phenomenon".
    double rough = 0.0;
    {
      sim_procgen::HashRng rr(poi_seed(now_day, system_id, 0xB17B17B1ULL));
      const int n = (cfg_.enable_nebula_microfields && neb_base > 1e-6) ? 6 : 0;
      for (int i = 0; i < n; ++i) {
        const Vec2 p = pick_site_near_jump(state_, system_id, rr, 35.0, 185.0);
        rough += grad_at(system_id, p);
      }
      if (n > 0) rough /= static_cast<double>(n);
    }

    const double w_phenom = (0.15 + 1.20 * neb_base + 0.90 * rough) * (1.0 - 0.22 * early_exploration_pressure);
    const double w_distortion = (0.10 + 1.30 * neb_base + 0.70 * rough) * (1.0 - 0.30 * early_exploration_pressure);
    const double w_xeno = 0.06 + 1.10 * rf.ruins + 0.20 * (1.0 - rf.pirate);
    const double w_signal = 0.45 + 0.40 * early_exploration_pressure;

    const double w_sum = w_ruins + w_distress + w_phenom + w_distortion + w_xeno + w_signal;
    const double u = rng.next_u01() * w_sum;

    if (u < w_ruins) {
      a.kind = AnomalyKind::Ruins;
      a.name = "";  // filled by procedural naming below
    } else if (u < w_ruins + w_distress) {
      a.kind = AnomalyKind::Distress;
      a.name = "";
    } else if (u < w_ruins + w_distress + w_phenom) {
      a.kind = AnomalyKind::Phenomenon;
      a.name = "";
    } else if (u < w_ruins + w_distress + w_phenom + w_distortion) {
      a.kind = AnomalyKind::Distortion;
      a.name = "";
    } else if (u < w_ruins + w_distress + w_phenom + w_distortion + w_xeno) {
      a.kind = AnomalyKind::Xenoarchaeology;
      a.name = "";
    } else {
      a.kind = AnomalyKind::Signal;
      a.name = "";
    }

    // --- Microfield-aware, soft blue-noise placement ---
    // We bias each anomaly kind toward a preferred local nebula density band,
    // plus optional filament edges (gradient). A soft separation term reduces
    // overlap with existing POIs without hard rejection loops.
    double target_d = 0.35;
    double w_d = 1.0;
    double w_g = 0.20;
    double w_s = 0.75;
    double min_sep = 18.0;
    int samples = 18;
    double r_min = 25.0;
    double r_max = 150.0;

    if (a.kind == AnomalyKind::Signal) {
      target_d = std::clamp(0.18 + 0.10 * (1.0 - neb_base), 0.05, 0.45);
      w_d = 1.25;
      w_g = 0.40;
      w_s = 0.70;
      min_sep = 16.0;
      samples = 18;
      r_min = 20.0;
      r_max = 140.0;
    } else if (a.kind == AnomalyKind::Distress) {
      target_d = std::clamp(0.32 + 0.18 * rf.pirate, 0.10, 0.70);
      w_d = 1.10;
      w_g = 0.35;
      w_s = 0.75;
      min_sep = 18.0;
      samples = 18;
      r_min = 25.0;
      r_max = 160.0;
    } else if (a.kind == AnomalyKind::Phenomenon) {
      target_d = std::clamp(0.40 + 0.25 * neb_base, 0.15, 0.85);
      w_d = 0.80;
      w_g = 1.25;
      w_s = 0.65;
      min_sep = 20.0;
      samples = 20;
      r_min = 35.0;
      r_max = 185.0;
    } else if (a.kind == AnomalyKind::Distortion) {
      target_d = std::clamp(0.50 + 0.30 * neb_base + 0.25 * rough, 0.22, 0.92);
      w_d = 1.10;
      w_g = 1.35;
      w_s = 0.75;
      min_sep = 19.0;
      samples = 22;
      r_min = 28.0;
      r_max = 190.0;
    } else if (a.kind == AnomalyKind::Xenoarchaeology) {
      target_d = std::clamp(0.56 + 0.16 * rf.ruins, 0.24, 0.88);
      w_d = 1.05;
      w_g = 0.75;
      w_s = 0.82;
      min_sep = 21.0;
      samples = 21;
      r_min = 30.0;
      r_max = 185.0;
    } else {  // ruins / artifact
      target_d = std::clamp(0.52 + 0.25 * rf.ruins + 0.10 * neb_base, 0.25, 0.90);
      w_d = 1.30;
      w_g = 0.25;
      w_s = 0.85;
      min_sep = 22.0;
      samples = 20;
      r_min = 45.0;
      r_max = 210.0;
    }

    // Early exploration quality-of-life:
    // - keep beginner-relevant sites closer to jump approaches
    // - reduce friction on first scouting arcs
    if (early_exploration_pressure > 1e-6) {
      const bool early_friendly_kind =
          (a.kind == AnomalyKind::Signal || a.kind == AnomalyKind::Distress || a.kind == AnomalyKind::Ruins ||
           a.kind == AnomalyKind::Xenoarchaeology || a.kind == AnomalyKind::Artifact);
      if (early_friendly_kind) {
        min_sep = std::max(12.0, min_sep * (0.90 - 0.08 * early_exploration_pressure));
        r_min = std::max(14.0, r_min * (0.78 - 0.08 * early_exploration_pressure));
        r_max = std::max(r_min + 24.0, r_max * (0.86 - 0.10 * early_exploration_pressure));
        w_s += 0.10 * early_exploration_pressure;
      } else {
        r_min = std::max(16.0, r_min * (0.90 - 0.04 * early_exploration_pressure));
        r_max = std::max(r_min + 30.0, r_max * (0.95 - 0.04 * early_exploration_pressure));
      }
      samples = std::clamp(samples + static_cast<int>(std::lround(2.0 * early_exploration_pressure)), 12, 30);
    }

    a.position_mkm = pick_biased_site(system_id, rng, occupied, target_d, w_d, w_g, w_s, min_sep, samples, r_min, r_max);
    occupied.push_back(a.position_mkm);

    const double neb = neb_at(system_id, a.position_mkm);
    const double grad = grad_at(system_id, a.position_mkm);

    // Obscure procedural naming (stable, deterministic per-site).
    // This gives anomalies unique identities without introducing new entity
    // fields or save format changes.
    a.name = procgen_obscure::generate_anomaly_name(a);
    const procgen_obscure::AnomalyScanReadout scan_profile =
        procgen_obscure::anomaly_scan_readout(a, neb, rf.ruins, rf.pirate);
    const procgen_obscure::AnomalySiteProfile site_profile =
        procgen_obscure::anomaly_site_profile(a, neb, rf.ruins, rf.pirate, grad);
    const procgen_obscure::AnomalyConvergenceProfile convergence_profile =
        procgen_obscure::anomaly_convergence_profile(a, scan_profile, site_profile, neb, rf.ruins, rf.pirate, grad);
    double convergence_link_chance = convergence_profile.link_chance;
    if (early_exploration_pressure > 1e-6) {
      const bool early_chain_kind =
          (a.kind == AnomalyKind::Signal || a.kind == AnomalyKind::Ruins || a.kind == AnomalyKind::Distress ||
           a.kind == AnomalyKind::Xenoarchaeology || a.kind == AnomalyKind::Artifact);
      if (early_chain_kind) {
        convergence_link_chance =
            std::clamp(convergence_link_chance + 0.18 * early_exploration_pressure, 0.0, 0.94);
      } else {
        convergence_link_chance =
            std::clamp(convergence_link_chance - 0.06 * early_exploration_pressure, 0.0, 0.88);
      }
    }
    bool linked_convergence = false;
    bool linked_domain_match = false;
    if (!state_.anomalies.empty() && convergence_link_chance > 1e-9) {
      const auto this_domain = procgen_obscure::anomaly_theme_domain(a);
      Id best_parent_id = kInvalidId;
      Id best_root_id = kInvalidId;
      int best_parent_depth = 0;
      double best_score = -1e9;

      for (const auto& [other_id, other] : state_.anomalies) {
        if (other_id == a.id) continue;
        if (other.system_id != system_id) continue;
        if (other.resolved) continue;

        const double d = (other.position_mkm - a.position_mkm).length();
        if (!std::isfinite(d)) continue;
        if (d > convergence_profile.link_radius_mkm) continue;

        const auto other_domain = procgen_obscure::anomaly_theme_domain(other);
        const bool domain_match = (other_domain == this_domain);
        const double near = 1.0 - std::clamp(d / std::max(1e-6, convergence_profile.link_radius_mkm), 0.0, 1.0);
        const double depth_norm = std::clamp(static_cast<double>(std::max(0, other.lead_depth)) / 6.0, 0.0, 1.0);
        const double score = 1.15 * near + (domain_match ? 0.45 : 0.0) + 0.20 * depth_norm + 0.02 * rng.next_u01();
        if (score > best_score) {
          best_score = score;
          best_parent_id = other.id;
          best_root_id = procgen_obscure::anomaly_chain_root_id(state_.anomalies, other.id);
          best_parent_depth = std::max(0, other.lead_depth);
          linked_domain_match = domain_match;
        }
      }

      if (best_parent_id != kInvalidId && rng.next_u01() < convergence_link_chance) {
        linked_convergence = true;
        a.origin_anomaly_id = (best_root_id != kInvalidId) ? best_root_id : best_parent_id;
        a.lead_depth = std::clamp(best_parent_depth + 1, 1, 12);
      }
    }
    if (rng.next_u01() < 0.22) {
      a.name += " {";
      a.name += procgen_obscure::anomaly_site_archetype_label(site_profile.archetype);
      a.name += "}";
    }
    if (linked_convergence && rng.next_u01() < 0.45) {
      a.name += " [Confluence]";
    }
    if (early_exploration_pressure >= 0.55 &&
        (a.kind == AnomalyKind::Signal || a.kind == AnomalyKind::Ruins || a.kind == AnomalyKind::Distress) &&
        rng.next_u01() < 0.24) {
      a.name += " [Pioneer]";
    }

    // Investigation time: longer in dense pockets, filament edges, and deeper ruins sites.
    const int base_days = 2 + rng.range_int(0, 5);
    const int neb_days = static_cast<int>(std::round(neb * 4.0));
    const int ruins_days = static_cast<int>(std::round(rf.ruins * 3.0));
    const int phen_days = (a.kind == AnomalyKind::Phenomenon) ? static_cast<int>(std::round(grad * 4.0)) : 0;
    const int dist_days = (a.kind == AnomalyKind::Distortion) ? static_cast<int>(std::round(1.5 + 2.0 * grad)) : 0;
    const int xeno_days = (a.kind == AnomalyKind::Xenoarchaeology) ? static_cast<int>(std::round(1.0 + 2.5 * rf.ruins)) : 0;
    a.investigation_days = std::clamp(base_days + neb_days + ruins_days + phen_days + dist_days, 1, 18);
    a.investigation_days = std::clamp(a.investigation_days + xeno_days, 1, 18);
    a.investigation_days =
        std::clamp(static_cast<int>(std::lround(static_cast<double>(a.investigation_days) * site_profile.investigation_mult)) +
                       site_profile.investigation_add_days,
                   1,
                   24);
    if (linked_convergence) {
      a.investigation_days = std::clamp(a.investigation_days + convergence_profile.extra_investigation_days, 1, 28);
    }
    {
      const bool early_friendly_kind =
          (a.kind == AnomalyKind::Signal || a.kind == AnomalyKind::Distress || a.kind == AnomalyKind::Ruins ||
           a.kind == AnomalyKind::Xenoarchaeology || a.kind == AnomalyKind::Artifact);
      const double relief_scale = early_friendly_kind ? 2.2 : 1.2;
      const int early_relief =
          static_cast<int>(std::lround(relief_scale * early_exploration_pressure * (1.0 - 0.30 * neb)));
      a.investigation_days = std::clamp(a.investigation_days - std::max(0, early_relief), 1, 28);
    }

    // Reward: research points plus optional minerals.
    double rp = rng.range(8.0, 42.0);
    rp *= (0.70 + 1.10 * rf.ruins);
    rp *= (0.80 + 0.40 * neb);
    rp *= (a.kind == AnomalyKind::Phenomenon) ? (0.85 + 0.45 * grad) : 1.0;
    rp *= (a.kind == AnomalyKind::Distress) ? (0.85 + 0.45 * rf.pirate) : 1.0;
    rp *= (a.kind == AnomalyKind::Distortion) ? (0.90 + 0.35 * rf.ruins + 0.40 * grad) : 1.0;
    rp *= (a.kind == AnomalyKind::Xenoarchaeology) ? (0.95 + 0.30 * rf.ruins + 0.15 * grad) : 1.0;
    rp *= site_profile.research_mult;
    if (linked_convergence) {
      rp *= convergence_profile.research_mult * (linked_domain_match ? 1.07 : 1.00);
    }
    {
      const bool early_friendly_kind =
          (a.kind == AnomalyKind::Signal || a.kind == AnomalyKind::Distress || a.kind == AnomalyKind::Ruins ||
           a.kind == AnomalyKind::Xenoarchaeology || a.kind == AnomalyKind::Artifact);
      const double onboarding_bonus = 1.0 + early_exploration_pressure * (early_friendly_kind ? 0.18 : 0.07);
      rp *= onboarding_bonus;
    }
    if (!std::isfinite(rp) || rp < 0.0) rp = 0.0;
    a.research_reward = rp;

    // Optional component unlock: rare, mostly in ruins/phenomena.
    const double unlock_chance =
        0.05 + 0.20 * rf.ruins + 0.05 * neb + 0.04 * grad + site_profile.unlock_bonus + (linked_convergence ? 0.03 : 0.0) +
        ((a.kind == AnomalyKind::Signal || a.kind == AnomalyKind::Ruins || a.kind == AnomalyKind::Xenoarchaeology)
             ? (0.05 * early_exploration_pressure)
             : (0.02 * early_exploration_pressure));
    if (rng.next_u01() < std::clamp(unlock_chance, 0.0, 0.35)) {
      a.unlock_component_id = sim_procgen::pick_any_component_id(content_, rng);
    }

    // Optional mineral cache.
    const double cache_chance = 0.25 + 0.35 * rf.ruins + 0.10 * rf.pirate +
                               (a.kind == AnomalyKind::Distortion ? 0.12 : 0.0) +
                               (a.kind == AnomalyKind::Xenoarchaeology ? 0.12 : 0.0) + site_profile.cache_bonus +
                               (linked_convergence ? convergence_profile.cache_bonus : 0.0) +
                               ((a.kind == AnomalyKind::Signal || a.kind == AnomalyKind::Distress) ? 0.08 : 0.03) *
                                   early_exploration_pressure;
    if (rng.next_u01() < std::clamp(cache_chance, 0.0, 0.85)) {
      const double scale = (0.8 + 1.2 * rf.ruins) * (0.7 + 0.6 * rf.salvage_mult) * (0.85 + 0.55 * neb) *
                           site_profile.mineral_mult * (linked_convergence ? convergence_profile.mineral_mult : 1.0);
      a.mineral_reward = sim_procgen::generate_mineral_bundle(rng, /*scale=*/1.4 * scale);
    }

    // Hazard: more likely in dense pockets and filament edges.
    const double hz_base =
        (a.kind == AnomalyKind::Phenomenon) ? 0.12
        : (a.kind == AnomalyKind::Distortion) ? 0.20
        : (a.kind == AnomalyKind::Xenoarchaeology) ? 0.10
                                        : 0.06;
    a.hazard_chance = std::clamp((hz_base + 0.28 * neb + 0.12 * grad) * site_profile.hazard_chance_mult *
                                     (linked_convergence ? convergence_profile.hazard_mult : 1.0),
                                 0.0,
                                 0.85);
    {
      const bool high_risk_kind = (a.kind == AnomalyKind::Distortion || a.kind == AnomalyKind::Phenomenon);
      const double early_hazard_relief = early_exploration_pressure * (high_risk_kind ? 0.08 : 0.22);
      a.hazard_chance = std::clamp(a.hazard_chance - early_hazard_relief, 0.0, 0.85);
    }
    if (a.hazard_chance > 1e-6) {
      a.hazard_damage =
          rng.range(0.6, 4.8) * (0.80 + 0.80 * neb) * (0.90 + 0.40 * grad) * site_profile.hazard_damage_mult *
          (linked_convergence ? convergence_profile.hazard_mult : 1.0);
      {
        const bool high_risk_kind = (a.kind == AnomalyKind::Distortion || a.kind == AnomalyKind::Phenomenon);
        const double early_damage_scale = 1.0 - early_exploration_pressure * (high_risk_kind ? 0.12 : 0.30);
        a.hazard_damage *= std::clamp(early_damage_scale, 0.55, 1.0);
      }
      if (!std::isfinite(a.hazard_damage) || a.hazard_damage < 0.0) a.hazard_damage = 0.0;
    }

    state_.anomalies[a.id] = std::move(a);
  };

  auto spawn_cache = [&](Id system_id, const StarSystem& sys, const RegionFactors& rf) {
    (void)sys;

    Wreck w;
    w.id = allocate_id(state_);
    w.system_id = system_id;
    w.kind = WreckKind::Cache;
    w.created_day = now_day;

    auto& occupied = occupied_by_sys[system_id];

    sim_procgen::HashRng rng(poi_seed(now_day, system_id, 0xCACECA5EULL));

    const double neb_base = clamp01(sys.nebula_density);

    // Placement: pirate caches hide deeper in dense pockets.
    double target_d = 0.30;
    if (rf.pirate > 0.55) target_d = 0.68;
    else if (rf.ruins > 0.55) target_d = 0.55;
    target_d = std::clamp(target_d + 0.12 * neb_base, 0.05, 0.90);

    w.position_mkm = pick_biased_site(system_id,
                                     rng,
                                     occupied,
                                     target_d,
                                     /*w_density=*/1.05,
                                     /*w_grad=*/0.20,
                                     /*w_sep=*/0.85,
                                     /*min_sep_mkm=*/14.0,
                                     /*samples=*/18,
                                     /*r_min_mkm=*/25.0,
                                     /*r_max_mkm=*/175.0);
    occupied.push_back(w.position_mkm);

    const double neb = neb_at(system_id, w.position_mkm);

    // Name can hint at origin (piracy/ruins risk) while still being unique.
    std::string tag;
    if (rf.pirate > 0.55) tag = "Pirate";
    else if (rf.ruins > 0.55) tag = "Ruins";
    else tag = "Drifting";
    w.name = procgen_obscure::generate_wreck_cache_name(w, tag);

    // Minerals scaled by salvage richness, pirate risk, and concealment.
    // (Dense pockets are harder to detect, so allow slightly better loot.)
    double scale = (1.0 + 0.8 * rf.pirate) * (0.75 + 0.75 * rf.salvage_mult);
    scale *= (0.80 + 0.60 * neb);
    w.minerals = sim_procgen::generate_mineral_bundle(rng, /*scale=*/2.1 * scale);

    // Ensure non-empty.
    if (w.minerals.empty()) {
      w.minerals["Duranium"] = 50.0;
    }

    state_.wrecks[w.id] = std::move(w);
  };

  const auto sys_ids = sorted_keys(state_.systems);
  for (Id sid : sys_ids) {
    if (unresolved_total >= max_anoms_total && caches_total >= max_caches_total) break;

    const auto* sys = find_ptr(state_.systems, sid);
    if (!sys) continue;

    const RegionFactors rf = region_factors_for_system(state_, *sys);
    const double neb = clamp01(sys->nebula_density);

    const bool has_col = system_has_colony(colony_systems, sid);

    // --- Anomaly spawn ---
    if (unresolved_total < max_anoms_total && base_anom_chance > 1e-12) {
      const int existing = (anoms_per_sys.find(sid) != anoms_per_sys.end()) ? anoms_per_sys[sid] : 0;
      const bool per_sys_ok = (per_sys_anom_cap <= 0) ? true : (existing < per_sys_anom_cap);

      if (per_sys_ok) {
        double p = base_anom_chance;
        p *= (0.25 + 1.75 * rf.ruins);
        p *= (0.90 + 0.25 * neb);
        if (has_col) p *= 0.35;  // keep colonies from being anomaly farms
        p *= 1.0 / (1.0 + 0.45 * static_cast<double>(existing));
        p = std::clamp(p, 0.0, 0.75);

        const double u = sim_procgen::u01_from_u64(poi_seed(now_day, sid, 0xA0A0A0A0ULL));
        if (u < p) {
          spawn_anomaly(sid, *sys, rf);
          ++unresolved_total;
          ++anoms_per_sys[sid];
        }
      }
    }

    // --- Cache spawn ---
    if (caches_total < max_caches_total && base_cache_chance > 1e-12) {
      const int existing = (caches_per_sys.find(sid) != caches_per_sys.end()) ? caches_per_sys[sid] : 0;
      const bool per_sys_ok = (per_sys_cache_cap <= 0) ? true : (existing < per_sys_cache_cap);

      if (per_sys_ok) {
        double p = base_cache_chance;
        p *= (0.15 + 1.10 * rf.pirate);
        p *= (0.80 + 0.20 * rf.ruins);
        // Dense nebula makes caches harder to find; reduce spawn slightly.
        p *= (0.95 - 0.25 * neb);
        if (has_col) p *= 0.60;
        p *= 1.0 / (1.0 + 0.55 * static_cast<double>(existing));
        p = std::clamp(p, 0.0, 0.60);

        const double u = sim_procgen::u01_from_u64(poi_seed(now_day, sid, 0xCAC0CAC0ULL));
        if (u < p) {
          spawn_cache(sid, *sys, rf);
          ++caches_total;
          ++caches_per_sys[sid];
        }
      }
    }
  }
}

} // namespace nebula4x
