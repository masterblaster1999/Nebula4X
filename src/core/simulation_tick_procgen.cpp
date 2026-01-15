#include "nebula4x/core/simulation.h"

#include "simulation_internal.h"
#include "simulation_procgen.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

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

  // Current unresolved anomaly counts.
  int unresolved_total = 0;
  std::unordered_map<Id, int> anoms_per_sys;
  anoms_per_sys.reserve(state_.anomalies.size() * 2 + 8);
  for (const auto& [aid, a] : state_.anomalies) {
    (void)aid;
    if (a.system_id == kInvalidId) continue;
    if (a.resolved) continue;
    ++unresolved_total;
    ++anoms_per_sys[a.system_id];
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
  }

  if (unresolved_total >= max_anoms_total && caches_total >= max_caches_total) return;

  auto spawn_anomaly = [&](Id system_id, const StarSystem& sys, const RegionFactors& rf) {
    Anomaly a;
    a.id = allocate_id(state_);
    a.system_id = system_id;

    sim_procgen::HashRng rng(poi_seed(now_day, system_id, 0xA11A11A1ULL));
    a.position_mkm = sim_procgen::pick_site_position_mkm(state_, system_id, rng);

    const double neb = clamp01(sys.nebula_density);

    // Choose a flavor (kind/name) influenced by region factors.
    const double w_ruins = 0.20 + 1.40 * rf.ruins;
    const double w_distress = 0.10 + 1.10 * rf.pirate;
    const double w_phenom = 0.15 + 1.20 * neb;
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

    // Obscure procedural naming (stable, deterministic per-site).
    // This gives anomalies unique identities without introducing new entity
    // fields or save format changes.
    a.name = procgen_obscure::generate_anomaly_name(a);

    // Investigation time: longer in heavy nebula and deeper "ruins" sites.
    const int base_days = 2 + rng.range_int(0, 5);
    const int neb_days = static_cast<int>(std::round(neb * 3.0));
    const int ruins_days = static_cast<int>(std::round(rf.ruins * 3.0));
    a.investigation_days = std::clamp(base_days + neb_days + ruins_days, 1, 16);

    // Reward: research points plus optional minerals.
    double rp = rng.range(8.0, 42.0);
    rp *= (0.70 + 1.10 * rf.ruins);
    rp *= (0.85 + 0.35 * neb);
    rp *= (a.kind == "distress") ? (0.85 + 0.45 * rf.pirate) : 1.0;
    if (!std::isfinite(rp) || rp < 0.0) rp = 0.0;
    a.research_reward = rp;

    // Optional component unlock: rare, mostly in ruins/phenomena.
    const double unlock_chance = 0.05 + 0.20 * rf.ruins + 0.05 * neb;
    if (rng.next_u01() < std::clamp(unlock_chance, 0.0, 0.35)) {
      a.unlock_component_id = sim_procgen::pick_any_component_id(content_, rng);
    }

    // Optional mineral cache.
    const double cache_chance = 0.25 + 0.35 * rf.ruins + 0.10 * rf.pirate;
    if (rng.next_u01() < std::clamp(cache_chance, 0.0, 0.85)) {
      const double scale = (0.8 + 1.2 * rf.ruins) * (0.7 + 0.6 * rf.salvage_mult);
      a.mineral_reward = sim_procgen::generate_mineral_bundle(rng, /*scale=*/1.4 * scale);
    }

    // Hazard: more likely in dense nebula and phenomena.
    const double hz_base = (a.kind == "phenomenon") ? 0.12 : 0.06;
    a.hazard_chance = std::clamp(hz_base + 0.25 * neb, 0.0, 0.65);
    if (a.hazard_chance > 1e-6) {
      a.hazard_damage = rng.range(0.6, 4.8) * (0.85 + 0.75 * neb);
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

    sim_procgen::HashRng rng(poi_seed(now_day, system_id, 0xCACECA5EULL));
    w.position_mkm = sim_procgen::pick_site_position_mkm(state_, system_id, rng);

    // Name can hint at origin (piracy/ruins risk) while still being unique.
    std::string tag;
    if (rf.pirate > 0.55) tag = "Pirate";
    else if (rf.ruins > 0.55) tag = "Ruins";
    else tag = "Drifting";
    w.name = procgen_obscure::generate_wreck_cache_name(w, tag);

    // Minerals scaled by salvage richness and a bit of pirate risk.
    const double scale = (1.0 + 0.8 * rf.pirate) * (0.75 + 0.75 * rf.salvage_mult);
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
