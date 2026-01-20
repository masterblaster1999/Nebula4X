#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

#include "nebula4x/core/simulation.h"
#include "nebula4x/util/hash_rng.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n";           \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

namespace {

static std::uint64_t poi_seed(std::int64_t day, nebula4x::Id system_id, std::uint64_t tag) {
  std::uint64_t s = static_cast<std::uint64_t>(day);
  s ^= (static_cast<std::uint64_t>(system_id) + 0x9e3779b97f4a7c15ULL) * 0xbf58476d1ce4e5b9ULL;
  s ^= tag * 0x94d049bb133111ebULL;
  return nebula4x::util::splitmix64(s);
}

static double clamp01(double v) {
  if (!std::isfinite(v)) return 0.0;
  return std::clamp(v, 0.0, 1.0);
}

} // namespace

int test_dynamic_poi_spawns() {
  using namespace nebula4x;

  // Minimal content: a single component so procgen rewards can reference a real id.
  ContentDB content;
  {
    ComponentDef c;
    c.id = "test_comp";
    c.name = "Recovered Test Component";
    c.type = ComponentType::Sensor;
    c.sensor_range_mkm = 10.0;
    content.components[c.id] = c;
  }

  SimConfig cfg;
  cfg.enable_dynamic_poi_spawns = true;
  cfg.dynamic_anomaly_spawn_chance_per_system_per_day = 1.0;
  cfg.dynamic_cache_spawn_chance_per_system_per_day = 1.0;
  cfg.dynamic_poi_max_unresolved_anomalies_total = 64;
  cfg.dynamic_poi_max_active_caches_total = 64;
  cfg.dynamic_poi_max_unresolved_anomalies_per_system = 8;
  cfg.dynamic_poi_max_active_caches_per_system = 8;

  Simulation sim(std::move(content), cfg);

  // Build a minimal state: 1 faction, 1 region, 1 system.
  GameState s;
  s.save_version = GameState{}.save_version;
  s.next_id = 1;
  const Date base_date = Date::from_ymd(2200, 1, 1);
  s.date = base_date;
  s.hour_of_day = 0;

  const Id fac_id = allocate_id(s);
  {
    Faction f;
    f.id = fac_id;
    f.name = "Test";
    f.control = FactionControl::Player;
    s.factions[fac_id] = f;
  }

  const Id reg_id = allocate_id(s);
  {
    Region r;
    r.id = reg_id;
    r.name = "Hot Region";
    r.ruins_density = 1.0;
    r.pirate_risk = 1.0;
    r.salvage_richness_mult = 1.0;
    s.regions[reg_id] = r;
  }

  const Id sys_id = allocate_id(s);
  {
    StarSystem sys;
    sys.id = sys_id;
    sys.name = "Procgen System";
    sys.region_id = reg_id;
    sys.nebula_density = 0.0;  // keep placement simple (no microfield roughness)
    sys.galaxy_pos = {0.0, 0.0};
    s.systems[sys_id] = sys;
  }

  // Pick an initial date such that the *next* day boundary triggers both an anomaly and a cache spawn.
  const std::int64_t base_day = s.date.days_since_epoch();

  const double rf_ruins = 1.0;
  const double rf_pirate = 1.0;
  const double neb = 0.0;

  const double p_anom = std::clamp(1.0 * (0.25 + 1.75 * rf_ruins) * (0.90 + 0.25 * neb), 0.0, 0.75);
  const double p_cache =
      std::clamp(1.0 * (0.15 + 1.10 * rf_pirate) * (0.80 + 0.20 * rf_ruins) * (0.95 - 0.25 * neb), 0.0, 0.60);

  int offset = 1;
  for (; offset < 256; ++offset) {
    const std::int64_t now_day = base_day + offset;
    const double u_anom = util::u01_from_u64(poi_seed(now_day, sys_id, 0xA0A0A0A0ULL));
    const double u_cache = util::u01_from_u64(poi_seed(now_day, sys_id, 0xCAC0CAC0ULL));
    if (u_anom < p_anom && u_cache < p_cache) break;
  }
  N4X_ASSERT(offset < 256);

  // Start the simulation on (base + offset - 1) so that advancing exactly 1 day lands on the desired now_day.
  s.date = base_date.add_days(offset - 1);

  sim.load_game(std::move(s));
  sim.advance_days(1);

  const auto& st = sim.state();
  const std::int64_t now_day = st.date.days_since_epoch();
  N4X_ASSERT(now_day == base_day + offset);

  // Exactly one system -> at most one anomaly + one cache per day.
  N4X_ASSERT(st.anomalies.size() == 1);
  N4X_ASSERT(st.wrecks.size() == 1);

  // Validate anomaly invariants.
  {
    const auto& a = st.anomalies.begin()->second;
    N4X_ASSERT(a.system_id == sys_id);
    N4X_ASSERT(!a.resolved);
    N4X_ASSERT(!a.kind.empty());
    N4X_ASSERT(!a.name.empty());
    N4X_ASSERT(a.investigation_days >= 1 && a.investigation_days <= 18);
    N4X_ASSERT(a.research_reward >= 0.0);
    N4X_ASSERT(clamp01(a.hazard_chance) >= 0.0);
    N4X_ASSERT(a.hazard_damage >= 0.0);
  }

  // Validate cache invariants.
  {
    const auto& w = st.wrecks.begin()->second;
    N4X_ASSERT(w.system_id == sys_id);
    N4X_ASSERT(w.kind == WreckKind::Cache);
    N4X_ASSERT(!w.name.empty());
    N4X_ASSERT(w.created_day == now_day);
    N4X_ASSERT(!w.minerals.empty());
  }

  return 0;
}
