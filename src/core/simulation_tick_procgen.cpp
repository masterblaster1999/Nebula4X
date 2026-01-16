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

  const double base_anom_chance = std::clamp(cfg_.dynamic_anomaly_spawn_chance_per_system_per_day, 0.0, 1.0);
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
    const double w_ruins = 0.20 + 1.40 * rf.ruins;
    const double w_distress = 0.10 + 1.10 * rf.pirate;

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

    const double w_phenom = 0.15 + 1.20 * neb_base + 0.90 * rough;
    const double w_signal = 0.55;

    const double w_sum = w_ruins + w_distress + w_phenom + w_signal;
    const double u = rng.next_u01() * w_sum;

    if (u < w_ruins) {
      a.kind = "ruins";
      a.name = "";  // filled by procedural naming below
    } else if (u < w_ruins + w_distress) {
      a.kind = "distress";
      a.name = "";
    } else if (u < w_ruins + w_distress + w_phenom) {
      a.kind = "phenomenon";
      a.name = "";
    } else {
      a.kind = "signal";
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

    if (a.kind == "signal") {
      target_d = std::clamp(0.18 + 0.10 * (1.0 - neb_base), 0.05, 0.45);
      w_d = 1.25;
      w_g = 0.40;
      w_s = 0.70;
      min_sep = 16.0;
      samples = 18;
      r_min = 20.0;
      r_max = 140.0;
    } else if (a.kind == "distress") {
      target_d = std::clamp(0.32 + 0.18 * rf.pirate, 0.10, 0.70);
      w_d = 1.10;
      w_g = 0.35;
      w_s = 0.75;
      min_sep = 18.0;
      samples = 18;
      r_min = 25.0;
      r_max = 160.0;
    } else if (a.kind == "phenomenon") {
      target_d = std::clamp(0.40 + 0.25 * neb_base, 0.15, 0.85);
      w_d = 0.80;
      w_g = 1.25;
      w_s = 0.65;
      min_sep = 20.0;
      samples = 20;
      r_min = 35.0;
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

    a.position_mkm = pick_biased_site(system_id, rng, occupied, target_d, w_d, w_g, w_s, min_sep, samples, r_min, r_max);
    occupied.push_back(a.position_mkm);

    const double neb = neb_at(system_id, a.position_mkm);
    const double grad = grad_at(system_id, a.position_mkm);

    // Obscure procedural naming (stable, deterministic per-site).
    // This gives anomalies unique identities without introducing new entity
    // fields or save format changes.
    a.name = procgen_obscure::generate_anomaly_name(a);

    // Investigation time: longer in dense pockets, filament edges, and deeper ruins sites.
    const int base_days = 2 + rng.range_int(0, 5);
    const int neb_days = static_cast<int>(std::round(neb * 4.0));
    const int ruins_days = static_cast<int>(std::round(rf.ruins * 3.0));
    const int phen_days = (a.kind == "phenomenon") ? static_cast<int>(std::round(grad * 4.0)) : 0;
    a.investigation_days = std::clamp(base_days + neb_days + ruins_days + phen_days, 1, 18);

    // Reward: research points plus optional minerals.
    double rp = rng.range(8.0, 42.0);
    rp *= (0.70 + 1.10 * rf.ruins);
    rp *= (0.80 + 0.40 * neb);
    rp *= (a.kind == "phenomenon") ? (0.85 + 0.45 * grad) : 1.0;
    rp *= (a.kind == "distress") ? (0.85 + 0.45 * rf.pirate) : 1.0;
    if (!std::isfinite(rp) || rp < 0.0) rp = 0.0;
    a.research_reward = rp;

    // Optional component unlock: rare, mostly in ruins/phenomena.
    const double unlock_chance = 0.05 + 0.20 * rf.ruins + 0.05 * neb + 0.04 * grad;
    if (rng.next_u01() < std::clamp(unlock_chance, 0.0, 0.35)) {
      a.unlock_component_id = sim_procgen::pick_any_component_id(content_, rng);
    }

    // Optional mineral cache.
    const double cache_chance = 0.25 + 0.35 * rf.ruins + 0.10 * rf.pirate;
    if (rng.next_u01() < std::clamp(cache_chance, 0.0, 0.85)) {
      const double scale = (0.8 + 1.2 * rf.ruins) * (0.7 + 0.6 * rf.salvage_mult) * (0.85 + 0.55 * neb);
      a.mineral_reward = sim_procgen::generate_mineral_bundle(rng, /*scale=*/1.4 * scale);
    }

    // Hazard: more likely in dense pockets and filament edges.
    const double hz_base = (a.kind == "phenomenon") ? 0.12 : 0.06;
    a.hazard_chance = std::clamp(hz_base + 0.28 * neb + 0.12 * grad, 0.0, 0.70);
    if (a.hazard_chance > 1e-6) {
      a.hazard_damage = rng.range(0.6, 4.8) * (0.80 + 0.80 * neb) * (0.90 + 0.40 * grad);
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
